// Diagnostics go to stderr, never stdout (stdout is the frame stream). One
// fwrite per line under a lock so the stdin reader thread, the frame loop and
// (later) shim code can log concurrently without interleaving mid-line.
//
// ADTRACE=<cat>,<cat>… enables trace categories; "all" / "*" enables every one.
// The set is process-global so shim code deep in a lane can ask tracing("gdi")
// without threading the Env through every call.
#pragma once

#include <set>
#include <string>
#include <string_view>

namespace adw {

// Prefix for log() lines, e.g. "adhostwin" -> "[adhostwin] ...".
void set_log_prefix(std::string_view prefix);
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

void set_trace_categories(const std::set<std::string>& cats);
bool tracing(std::string_view cat);
// "[cat] ..." — only when tracing(cat).
void trace(std::string_view cat, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Raw line to stderr (adds nothing; caller supplies '\n'). Used for the
// machine-parsed FBHASH lines so they carry no prefix.
void write_stderr(std::string_view text);

}  // namespace adw
