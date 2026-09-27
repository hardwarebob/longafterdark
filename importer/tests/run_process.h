// Runs a program with arguments, no console window, and returns its exit
// code plus everything it wrote to stdout and stderr (merged). A program
// still running after `timeout_ms` is killed (exit code -2) so a stray modal
// window can never wedge a test run.
#pragma once

#include <windows.h>

#include <string>
#include <thread>
#include <vector>

#include "winutil.h"

namespace test {

struct ProcessResult {
  int exit_code = -1;
  std::string output;
};

// One argument quoted so CommandLineToArgvW / the CRT hand it back intact:
// backslashes are literal except before a quote, so a run of them followed by
// a quote (or by the closing quote — think "E:\") is doubled.
inline std::wstring quote_arg(const std::wstring& a) {
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      slashes++;
      continue;
    }
    out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    out += c;
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}

inline std::wstring command_line(const std::wstring& exe, const std::vector<std::wstring>& args) {
  std::wstring cmd = quote_arg(exe);
  for (const std::wstring& a : args) cmd += L" " + quote_arg(a);
  return cmd;
}

inline ProcessResult run_process(const std::wstring& exe, const std::vector<std::wstring>& args,
                                 DWORD timeout_ms = 30 * 60 * 1000) {
  std::wstring cmd = command_line(exe, args);
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  CreatePipe(&rd, &wr, &sa, 0);
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = wr;
  si.hStdError = wr;
  PROCESS_INFORMATION pi{};
  ProcessResult r;
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                      &pi)) {
    r.output = "CreateProcess failed: " + adw::import::win_error_string(GetLastError());
    CloseHandle(rd);
    CloseHandle(wr);
    return r;
  }
  CloseHandle(wr);
  std::thread reader([&] {
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) r.output.append(buf, got);
  });
  bool timed_out = WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_TIMEOUT;
  if (timed_out) {
    TerminateProcess(pi.hProcess, DWORD(-2));
    WaitForSingleObject(pi.hProcess, INFINITE);
  }
  reader.join();
  CloseHandle(rd);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  r.exit_code = timed_out ? -2 : int(code);
  if (timed_out) r.output += "\n(killed after timeout)\n";
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return r;
}

}  // namespace test
