// Optional diagnostic log. Off unless AD_SCR_LOG names a file; the smoke
// tests turn it on to assert what the saver decided (spawns, respawns,
// rotations) without scraping the screen.
//
// The last-exit log (INTERACTION.md §9.1) is always on for /s: every event
// that explains how a run ended (start, spawns, host exits, respawns,
// rotations, the exit reason) goes to logs\saver-last.log next to
// settings.ini, rewritten per /s run and capped at kLastLogMaxLines (the
// first lines and the latest ones are kept; the middle gives way).
#pragma once

#include <string>

namespace adw::scr {

void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

inline constexpr size_t kLastLogMaxLines = 200;
// Starts the last-exit log at `path` (truncated; the folder is created).
void last_log_open(const std::wstring& path);
// One event: to the last-exit log (when open) and to log_line().
void last_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// The lines currently kept (tests).
size_t last_log_lines();

} // namespace adw::scr
