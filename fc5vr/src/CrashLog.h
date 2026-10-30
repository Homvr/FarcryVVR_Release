#pragma once

// Crash logger at launch (research\fc5_crash_logger_plan_2026-10-22.md): launch steps, the unhandled-exception filter
// with its own crash report (fc5vr_logs\crash_<date>.txt + .dmp), the boot header and the start watchdog.
#include <windows.h>
#include <string>

namespace fc5::crash {
// DllMain, before the init thread: our filter, the CRT handlers, the report thread, the crash test file.
void install_early(HMODULE self);
// Once fc5vr_log.txt is open: where crash reports go (its fc5vr_logs folder), and the steps taken before the log.
void log_ready(const std::wstring& log_path);
// Keeps the newest 3 dumps and 5 crash reports in fc5vr_logs.
void cleanup_old_reports();
// Build, exe, Windows, GPUs, OpenXR runtime and layers, DLLs in bin, VC++ runtime, ViGEmBus.
void log_boot_header();
// Until the success line: a note with stacks when no new start step comes for 30 s (again at 120 s).
void start_boot_watchdog();
void step(const char* name);
// DllMain DLL_PROCESS_DETACH: a "[Boot] process exit" line straight into the log (normal: lpReserved set, the process is ending).
void process_exit(bool normal);
}
