#pragma once

// Far Cry 5 VR: launch steps and the crash handler live in the mod (src/CrashLog.cpp); vrframework reports its start
// steps through these plain functions. Names must be string literals (the step ring keeps the pointer).
#include <windows.h>

void fc5_boot_step(const char* name);
// The result of the step just started on this thread with the same name (an XrResult, HRESULT...), logged with it.
void fc5_boot_result(const char* name, long result);
// The first frame the headset accepted (xrEndFrame succeeded): the "[Boot] VR running" success line.
void fc5_boot_vr_running();
// Puts the mod's unhandled-exception filter back on top (chaining to whoever replaced it).
void fc5_crash_reassert(const char* where);
// In a catch(...): the builds use /EHa, so catch(...) also catches access violations; this logs the last such
// exception this thread raised (code, place, stack), or says it was a C++ exception.
void fc5_crash_log_caught(const char* where);
