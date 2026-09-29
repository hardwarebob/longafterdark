#include "dialog_support.h"

#include <algorithm>
#include <cwchar>
#include <vector>

#include "host_process.h"
#include "paths.h"
#include "sound.h"

namespace adw::scr {

namespace {

constexpr std::wstring_view kPreviewPrefix = L"LongAfterDark-preview-";
constexpr std::wstring_view kPreviewSuffix = L".ini";

bool iequals_w(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() &&
         CompareStringOrdinal(a.data(), (int)a.size(), b.data(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

// A pid that names no running process. Access denied means it runs (as
// someone else); a handle to a process that has exited means it doesn't.
bool process_gone(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return GetLastError() == ERROR_INVALID_PARAMETER;
  DWORD code = 0;
  bool gone = GetExitCodeProcess(h, &code) && code != STILL_ACTIVE;
  CloseHandle(h);
  return gone;
}

} // namespace

// ---- helper programs ------------------------------------------------------------

DWORD child_creation_flags(const ChildLaunch& c) {
  DWORD f = 0;
  if (!c.env_block.empty()) f |= CREATE_UNICODE_ENVIRONMENT;
  if (c.console_program) f |= CREATE_NO_WINDOW;
  return f;
}

HANDLE start_child(const ChildLaunch& c, DWORD* error) {
  std::wstring cmd = quote_arg(c.exe) + (c.args.empty() ? L"" : L" " + c.args);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(c.exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, child_creation_flags(c),
                      c.env_block.empty() ? nullptr : const_cast<wchar_t*>(c.env_block.data()), nullptr, &si, &pi)) {
    if (error) *error = GetLastError();
    return nullptr;
  }
  CloseHandle(pi.hThread);
  if (error) *error = 0;
  return pi.hProcess;
}

ImportOutcome classify_import_exit(DWORD code) {
  if (code == 0) return ImportOutcome::imported;
  if (code == 5) return ImportOutcome::cancelled;
  return ImportOutcome::failed;
}

std::wstring import_outcome_note(DWORD code) {
  switch (classify_import_exit(code)) {
    case ImportOutcome::imported:
      return {};
    case ImportOutcome::cancelled:
      return L"Import cancelled. Nothing was changed.";
    case ImportOutcome::failed:
      break;
  }
  // adimport has already shown why; this only records that it didn't happen.
  // A crash ends it with an NTSTATUS, recognisable only in hex.
  wchar_t num[16];
  swprintf(num, 16, code >= 0x10000 ? L"0x%08lX" : L"%lu", (unsigned long)code);
  return L"Import did not finish (adimport exit code " + std::wstring(num) + L"). Nothing was changed.";
}

// ---- what the host can do ----------------------------------------------------------

namespace {

std::vector<std::string> split_list(const std::string& v) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : v + ",") {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

bool HostCapabilities::has_lane(const std::string& lane) const { return contains(lanes, lane); }
bool HostCapabilities::has_abi(const std::string& abi) const { return contains(abis, abi); }
bool HostCapabilities::can_configure(const std::string& lane) const { return contains(configure, lane); }

bool HostCapabilities::runs(const std::string& lane, const std::string& abi) const {
  return !known || (has_lane(lane) && has_abi(abi.empty() ? std::string(kAfterDarkAbi) : abi));
}

ModuleRun module_run(const Module& m, const HostCapabilities& caps, bool probing, bool exited_3) {
  if (exited_3) return ModuleRun::coming_soon;
  if (probing) return ModuleRun::waiting;
  return caps.runs(m.lane, m.abi) ? ModuleRun::runs : ModuleRun::coming_soon;
}

HostCapabilities parse_capabilities(const std::string& text) {
  HostCapabilities c;
  std::string line = text.substr(0, text.find_first_of("\r\n"));
  c.line = line;
  bool any = false;
  size_t pos = 0;
  while (pos < line.size()) {
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    size_t end = line.find_first_of(" \t", pos);
    std::string tok = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? line.size() : end;
    size_t eq = tok.find('=');
    if (eq == std::string::npos) continue;
    std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
    if (k == "lanes") {
      c.lanes = split_list(v);
      any = true;
    } else if (k == "configure") {
      c.configure = split_list(v);
    } else if (k == "abis") {
      c.abis = split_list(v);   // as listed: "abis=" alone runs no module ABI at all
    } else if (k == "status") {
      c.status = v == "1";
    } else if (k == "state") {
      c.state = v == "1";
    } else if (k == "seed") {
      c.seed = v == "1";
    } else if (k == "numlock") {
      c.numlock = v == "1";
    }
  }
  c.known = any;
  return c;
}

std::pair<std::wstring, std::wstring> numlock_env(const HostCapabilities& caps, bool on) {
  if (!caps.takes_numlock_env()) return {L"ADNUMLOCK", L""};
  return {L"ADNUMLOCK", on ? L"1" : L"0"};
}

HostTool::~HostTool() {
  if (stdout_r) CloseHandle(stdout_r);
  if (process) CloseHandle(process);
  if (job) CloseHandle(job);   // KILL_ON_JOB_CLOSE: nothing outlives us
}

bool HostTool::start(const std::wstring& exe, const std::wstring& args_tail,
                     const std::vector<std::pair<std::wstring, std::wstring>>& env_changes, std::wstring* error) {
  auto fail = [&](const wchar_t* what, DWORD err) {
    if (error) *error = std::wstring(what) + L" failed (error " + std::to_wstring(err) + L")";
    return false;
  };
  if (!file_exists(exe)) return fail(L"finding the host", ERROR_FILE_NOT_FOUND);
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, 0, nullptr);
  HANDLE err_h = INVALID_HANDLE_VALUE;
  if (std::wstring log = env_w(L"AD_SCR_HOSTLOG"); !log.empty()) {
    err_h = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  HANDLE out_w = nullptr;
  if (!CreatePipe(&stdout_r, &out_w, &sa, 1 << 16)) {
    DWORD e = GetLastError();
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (err_h != INVALID_HANDLE_VALUE) CloseHandle(err_h);
    return fail(L"CreatePipe", e);
  }
  SetHandleInformation(stdout_r, HANDLE_FLAG_INHERIT, 0);
  const HANDLE err_use = err_h != INVALID_HANDLE_VALUE ? err_h : nul;

  // Exactly these handles are inherited, never a live preview's pipes.
  HANDLE inherit[3];
  size_t n = 0;
  if (nul != INVALID_HANDLE_VALUE) inherit[n++] = nul;
  inherit[n++] = out_w;
  if (err_h != INVALID_HANDLE_VALUE) inherit[n++] = err_h;
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 2, 0, &attr_size);
  std::vector<char> attr_buf(attr_size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  job = create_kill_on_close_job();
  const bool attrs_init = attr_size && InitializeProcThreadAttributeList(attrs, 2, 0, &attr_size);
  const bool have_attrs = attrs_init && UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                                                  n * sizeof(HANDLE), nullptr, nullptr);
  const bool in_job = have_attrs && job &&
                      UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job, sizeof(job), nullptr, nullptr);
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
  si.StartupInfo.hStdOutput = out_w;
  si.StartupInfo.hStdError = err_use != INVALID_HANDLE_VALUE ? err_use : nullptr;
  si.lpAttributeList = have_attrs ? attrs : nullptr;

  std::vector<std::pair<std::wstring, std::wstring>> env = env_changes;
  // --configure and --capabilities never play (AUDIO.md §9).
  add_sound_env(env, sound_for(Settings{}, HostRole::tool, false, false));
  add_host_defaults(env);
  // Never a stale inherited status handle, stream request or frame limit.
  env.emplace_back(L"ADSTATUSHANDLE", L"");
  env.emplace_back(L"ADSTREAM", L"");
  std::wstring block = build_environment_block(env);
  std::wstring cmd = quote_arg(exe) + (args_tail.empty() ? L"" : L" " + args_tail);
  PROCESS_INFORMATION pi{};
  DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
  if (have_attrs) flags |= EXTENDED_STARTUPINFO_PRESENT;
  BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, block.data(), nullptr,
                           &si.StartupInfo, &pi);
  DWORD create_err = GetLastError();
  if (attrs_init) DeleteProcThreadAttributeList(attrs);
  CloseHandle(out_w);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (err_h != INVALID_HANDLE_VALUE) CloseHandle(err_h);
  if (!ok) return fail(L"CreateProcess", create_err);
  if (job && !in_job) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  process = pi.hProcess;
  pid = pi.dwProcessId;
  return true;
}

bool HostTool::finish(DWORD timeout_ms, std::string* out, DWORD* code) {
  if (!process) return false;
  const ULONGLONG t0 = GetTickCount64();
  auto drain = [&] {
    for (;;) {
      DWORD avail = 0;
      if (!stdout_r || !PeekNamedPipe(stdout_r, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return;
      std::string chunk(avail, '\0');
      DWORD got = 0;
      if (!ReadFile(stdout_r, chunk.data(), avail, &got, nullptr) || got == 0) return;
      if (out && out->size() < (1u << 20)) out->append(chunk.data(), got);
    }
  };
  bool exited = false;
  for (;;) {
    exited = WaitForSingleObject(process, 20) == WAIT_OBJECT_0;
    drain();
    if (exited) break;
    if (timeout_ms != INFINITE && GetTickCount64() - t0 >= timeout_ms) {
      TerminateProcess(process, 1);
      WaitForSingleObject(process, 2000);
      break;
    }
  }
  drain();
  DWORD c = STILL_ACTIVE;
  GetExitCodeProcess(process, &c);
  if (code) *code = c;
  return exited;
}

HostCapabilities probe_capabilities(const std::wstring& host_exe, DWORD timeout_ms) {
  HostTool t;
  if (!t.start(host_exe, L"--capabilities", {}, nullptr)) return {};
  std::string out;
  DWORD code = 1;
  if (!t.finish(timeout_ms, &out, &code) || code != 0) return {};
  return parse_capabilities(out);
}

std::wstring configure_args(const std::wstring& module_path, int slot, uintptr_t owner) {
  return L"--configure " + quote_arg(module_path) + L" --button " + std::to_wstring(slot) + L" --owner " +
         std::to_wstring((unsigned long long)owner);
}

std::wstring configure_outcome_note(DWORD code) {
  if (code == 0) return {};
  if (code == 4) return L"Nothing to set here";
  // A crash ends the host with an NTSTATUS, recognisable only in hex; the
  // host's own codes (1, 3, 5) read as decimal.
  wchar_t num[16];
  swprintf(num, 16, code >= 0x10000 ? L"0x%08lX" : L"%lu", (unsigned long)code);
  return L"Couldn’t open this option (code " + std::wstring(num) + L")";
}

// ---- the Preview's settings file ---------------------------------------------------

std::wstring preview_settings_path(const std::wstring& dir, DWORD dialog_pid) {
  return join_path(dir, std::wstring(kPreviewPrefix) + std::to_wstring(dialog_pid) + std::wstring(kPreviewSuffix));
}

bool parse_preview_settings_name(std::wstring_view name, DWORD* pid) {
  if (name.size() <= kPreviewPrefix.size() + kPreviewSuffix.size()) return false;
  if (!iequals_w(name.substr(0, kPreviewPrefix.size()), kPreviewPrefix)) return false;
  if (!iequals_w(name.substr(name.size() - kPreviewSuffix.size()), kPreviewSuffix)) return false;
  std::wstring_view digits =
      name.substr(kPreviewPrefix.size(), name.size() - kPreviewPrefix.size() - kPreviewSuffix.size());
  if (digits.size() > 10) return false;
  unsigned long long v = 0;
  for (wchar_t c : digits) {
    if (c < L'0' || c > L'9') return false;
    v = v * 10 + (c - L'0');
  }
  if (v > 0xFFFFFFFFull) return false;
  if (pid) *pid = (DWORD)v;
  return true;
}

int sweep_stale_preview_settings(const std::wstring& dir) {
  int removed = 0;
  WIN32_FIND_DATAW fd;
  HANDLE f = FindFirstFileExW(join_path(dir, std::wstring(kPreviewPrefix) + L"*").c_str(), FindExInfoBasic, &fd,
                              FindExSearchNameMatch, nullptr, 0);
  if (f == INVALID_HANDLE_VALUE) return 0;
  do {
    DWORD pid = 0;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    if (!parse_preview_settings_name(fd.cFileName, &pid)) continue;
    if (pid == GetCurrentProcessId() || !process_gone(pid)) continue;
    if (DeleteFileW(join_path(dir, fd.cFileName).c_str())) ++removed;
  } while (FindNextFileW(f, &fd));
  FindClose(f);
  return removed;
}

std::wstring temp_dir() {
  wchar_t buf[MAX_PATH + 2] = {};
  DWORD n = GetTempPathW(MAX_PATH + 1, buf);
  std::wstring d(buf, n > 0 && n <= MAX_PATH + 1 ? n : 0);
  while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
  return d;
}

} // namespace adw::scr
