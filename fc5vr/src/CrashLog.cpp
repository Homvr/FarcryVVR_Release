// Crash logger at launch (research\fc5_crash_logger_plan_2026-10-22.md).
//
// Testers whose game closes before VR starts had nothing useful to send: the log opened 5-20 s after load (so an early
// crash left the previous run's log), vrframework's crash handler only came at the first present and wrote a plain
// dump, and nothing said how far the start got. Here:
//   - launch steps: a 64-entry ring {ms since load, thread, name}, each also logged "[Boot] step N name (+ms, thread)";
//     "[Boot] VR running: first frame in the headset" is the success line the launcher looks for;
//   - our unhandled-exception filter from DllMain, chained to the previous one (and to whoever replaces it later), so
//     Windows still logs its Application Error event; std::terminate, pure calls and abort() raise our own codes;
//   - the crash report fc5vr_logs\crash_<date>.txt (WriteFile only: no spdlog, no heap) and crash_<date>.dmp, written
//     by a helper thread (a stack overflow leaves the crashing thread no stack to work with);
//   - the boot header (build, exe, Windows, GPUs, OpenXR runtime and layers, DLLs in bin, VC++ runtime, ViGEmBus);
//   - a start watchdog: no new step for 30 s (and 120 s) logs where the start is stuck, with stacks.
// bin\fc5vr_crashtest.txt "<kind> <step name prefix>" (av, stack, abort, fastfail, throw, terminate, hang) crashes on
// purpose when that step is reached; the file is deleted once read.
#include "CrashLog.h"
#include "fc5vr_build.h"

#include <Fc5Boot.hpp>
#include <spdlog/spdlog.h>

#include <windows.h>
#include <winternl.h>
#include <dbghelp.h>
#include <intrin.h>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "version.lib")

namespace {
constexpr DWORD kCodeTerminate = 0xE0FC5001;   // std::terminate called
constexpr DWORD kCodePurecall  = 0xE0FC5002;   // pure virtual function call
constexpr DWORD kCodeAbort     = 0xE0FC5003;   // abort() called (SIGABRT)

struct Step
{
    uint32_t    ms;
    DWORD       tid;
    const char* name;
    long        result;
    bool        has_result;
};
constexpr uint32_t    kRing = 64;
Step                  g_ring[kRing]{};
std::atomic<uint32_t> g_steps{ 0 };
std::atomic<uint64_t> g_last_step_tick{ 0 };
uint64_t              g_load_tick = 0;
std::atomic<bool>     g_log_ready{ false };
std::atomic<bool>     g_vr_running{ false };

HMODULE                      g_self = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;
wchar_t                      g_exe_dir[MAX_PATH]{};   // with the trailing backslash
wchar_t                      g_dir[MAX_PATH]{};       // fc5vr_logs\, with the trailing backslash
wchar_t                      g_log_path[MAX_PATH]{};
wchar_t                      g_windir[MAX_PATH]{};    // C:\Windows\ (modules under it are Windows')
size_t                       g_windir_len = 0;
char                         g_link_time[32] = "?";   // dxgi.dll's link time (PE header), local

// Notes from before the log was open (filter replaced in DllMain time...), logged by log_ready.
char             g_early_notes[4][160]{};
std::atomic<int> g_early_note_count{ 0 };

// The crash test (fc5vr_crashtest.txt).
char              g_ct_kind[16]{};
char              g_ct_prefix[96]{};
std::atomic<bool> g_ct_armed{ false };

// The report thread and its hand-over.
HANDLE              g_req_evt = nullptr, g_done_evt = nullptr, g_flush_evt = nullptr;
std::atomic<bool>   g_helper_running{ false };
std::atomic<int>    g_reporting{ 0 };
EXCEPTION_POINTERS* g_ep        = nullptr;
DWORD               g_crash_tid = 0;
thread_local int    t_in_filter = 0;

uint32_t ms_since_load() { return (uint32_t)(GetTickCount64() - g_load_tick); }

void early_note(const char* text)
{
    const int i = g_early_note_count.fetch_add(1);
    if (i < 4) strncpy_s(g_early_notes[i], text, _TRUNCATE);
}

// ---- no-heap helpers (used while crashing) ----

// A plain buffered writer on a Win32 file handle.
struct Out
{
    HANDLE h = INVALID_HANDLE_VALUE;
    char   buf[8192];
    int    n = 0;
    void   flush()
    {
        if (n > 0 && h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, buf, (DWORD)n, &w, nullptr);
        }
        n = 0;
    }
    void put(const char* fmt, ...)
    {
        char    line[1024];
        va_list a;
        va_start(a, fmt);
        int len = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, a);
        va_end(a);
        if (len < 0) len = (int)strlen(line);
        if (n + len > (int)sizeof(buf)) flush();
        memcpy(buf + n, line, (size_t)len);
        n += len;
    }
};

void narrow(const wchar_t* w, int wlen, char* out, int out_size)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, out, out_size - 1, nullptr, nullptr);
    if (n < 0) n = 0;
    out[n] = 0;
}

// The loaded module holding an address, from the loader's list in the PEB, read without the loader lock (the crashing
// thread may hold it).
struct ModInfo
{
    uintptr_t base = 0;
    uint32_t  size = 0;
    char      path[MAX_PATH]{};
    const char* name() const
    {
        const char* s = strrchr(path, '\\');
        return s ? s + 1 : path;
    }
};

bool is_windows_path(const wchar_t* p, size_t len) { return g_windir_len > 0 && len >= g_windir_len && _wcsnicmp(p, g_windir, g_windir_len) == 0; }

// Calls fn(entry) for each module in load order; stops when fn returns true. Under __try (the list may be changing).
template <typename F> void for_each_module(F&& fn);

bool module_entry(LIST_ENTRY* e, uintptr_t& base, uint32_t& size, const wchar_t*& path, size_t& len)
{
    __try {
        auto* m = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        base    = (uintptr_t)m->DllBase;
        size    = (uint32_t)(uintptr_t)m->Reserved3[1];   // SizeOfImage
        path    = m->FullDllName.Buffer;
        len     = m->FullDllName.Length / sizeof(wchar_t);
        return base != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

LIST_ENTRY* module_next(LIST_ENTRY* e)
{
    __try {
        return e->Flink;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

LIST_ENTRY* module_list_head()
{
    __try {
        auto* peb = reinterpret_cast<PPEB>(__readgsqword(0x60));
        return &peb->Ldr->InMemoryOrderModuleList;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

template <typename F> void for_each_module(F&& fn)
{
    LIST_ENTRY* head = module_list_head();
    if (!head) return;
    LIST_ENTRY* e = module_next(head);
    for (int guard = 0; e && e != head && guard < 2048; ++guard) {
        uintptr_t      base = 0;
        uint32_t       size = 0;
        const wchar_t* path = nullptr;
        size_t         len  = 0;
        if (module_entry(e, base, size, path, len)) {
            if (fn(base, size, path, len)) return;
        }
        e = module_next(e);
    }
}

bool find_module(uintptr_t addr, ModInfo& out)
{
    bool found = false;
    for_each_module([&](uintptr_t base, uint32_t size, const wchar_t* path, size_t len) {
        if (addr >= base && addr < base + size) {
            out.base = base;
            out.size = size;
            __try {
                narrow(path, (int)len, out.path, MAX_PATH);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                strcpy_s(out.path, "?");
            }
            found = true;
            return true;
        }
        return false;
    });
    return found;
}

uint32_t pe_timestamp(uintptr_t base)
{
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return nt->FileHeader.TimeDateStamp;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// "module+0xoffset" for an address.
void where(uintptr_t addr, char* out, size_t out_size)
{
    ModInfo m;
    if (find_module(addr, m)) _snprintf_s(out, out_size, _TRUNCATE, "%s+0x%llx", m.name(), (unsigned long long)(addr - m.base));
    else _snprintf_s(out, out_size, _TRUNCATE, "0x%llx (no module)", (unsigned long long)addr);
}

// Walks a stack from a context with the unwind data (x64: every non-leaf function has it). No heap.
int unwind_one(CONTEXT& c)
{
    __try {
        DWORD64 image_base = 0;
        auto*   fe         = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
        if (fe) {
            PVOID   handler_data = nullptr;
            DWORD64 establisher  = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fe, &c, &handler_data, &establisher, nullptr);
        } else {
            // A leaf function: the return address is on top of the stack.
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
        }
        return c.Rip != 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int walk_stack(const CONTEXT& from, DWORD64* pcs, int max)
{
    CONTEXT c = from;
    int     n = 0;
    if (c.Rip == 0) return 0;
    pcs[n++] = c.Rip;
    while (n < max) {
        const DWORD64 rsp = c.Rsp;
        if (!unwind_one(c)) break;
        if (c.Rsp <= rsp && c.Rip == pcs[n - 1]) break;   // no progress
        pcs[n++] = c.Rip;
    }
    return n;
}

const char* code_name(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error (a file or the page file could not be read)";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "float invalid operation";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_SINGLE_STEP: return "single step";
    case EXCEPTION_INVALID_HANDLE: return "invalid handle";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "non-continuable exception";
    case 0xC0000409: return "stack buffer overrun / fast fail";
    case 0xC0000374: return "heap corruption";
    case 0xC0000017: return "out of memory";
    case 0xC0000420: return "assertion failure";
    case 0xC000041D: return "exception in a user callback";
    case 0xE06D7363: return "C++ exception nobody caught";
    case kCodeTerminate: return "std::terminate called (C++ exception or noexcept violation)";
    case kCodePurecall: return "pure virtual function call";
    case kCodeAbort: return "abort() called";
    default: return "unknown exception";
    }
}

// For a C++ exception: the thrown type's decorated name and, for a std::exception, what().
void describe_cpp(const EXCEPTION_RECORD* r, char* type, size_t type_size, char* what, size_t what_size)
{
    type[0] = what[0] = 0;
    __try {
        if (r->NumberParameters < 4) return;
        const auto obj        = r->ExceptionInformation[1];
        const auto throw_info = r->ExceptionInformation[2];
        const auto image      = r->ExceptionInformation[3];
        if (!throw_info || !image) return;
        const int32_t cta_rva = *reinterpret_cast<const int32_t*>(throw_info + 12);
        const auto    cta     = image + cta_rva;
        const int32_t count   = *reinterpret_cast<const int32_t*>(cta);
        bool          is_std  = false;
        for (int32_t i = 0; i < count && i < 16; ++i) {
            const int32_t ct_rva = *reinterpret_cast<const int32_t*>(cta + 4 + 4 * i);
            const int32_t td_rva = *reinterpret_cast<const int32_t*>(image + ct_rva + 4);
            const char*   name   = reinterpret_cast<const char*>(image + td_rva + 16);
            if (i == 0) strncpy_s(type, type_size, name, _TRUNCATE);
            if (strcmp(name, ".?AVexception@std@@") == 0) is_std = true;
        }
        if (is_std && obj) {
            const char* w = reinterpret_cast<const std::exception*>(obj)->what();
            if (w) strncpy_s(what, what_size, w, _TRUNCATE);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void stamp(char* out, size_t size)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    _snprintf_s(out, size, _TRUNCATE, "%04u-%02u-%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

void put_step(Out& o, uint32_t i, const Step& s)
{
    char res[48]{};
    if (s.has_result) _snprintf_s(res, sizeof(res), _TRUNCATE, ": result %ld (0x%08lx)", s.result, (unsigned long)s.result);
    o.put("  step %u %s%s (+%u ms, thread %lu)\r\n", i + 1, s.name ? s.name : "?", res, s.ms, (unsigned long)s.tid);
}

// The newest step a thread took (index, or -1).
int last_step_of(DWORD tid)
{
    const uint32_t n = g_steps.load();
    for (uint32_t k = 0; k < kRing && k < n; ++k) {
        const uint32_t i = n - 1 - k;
        if (g_ring[i % kRing].tid == tid) return (int)i;
    }
    return -1;
}

void put_modules(Out& o)
{
    for_each_module([&](uintptr_t base, uint32_t size, const wchar_t* path, size_t len) {
        char p[MAX_PATH]{};
        bool windows = false;
        __try {
            windows = is_windows_path(path, len);
            if (!windows) narrow(path, (int)len, p, MAX_PATH);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            strcpy_s(p, "?");
        }
        if (!windows) o.put("  %016llx %08x %08x %s\r\n", (unsigned long long)base, size, pe_timestamp(base), p);
        return false;
    });
}

void write_report(EXCEPTION_POINTERS* ep, DWORD tid, wchar_t* dmp_path_out)
{
    char st[32];
    stamp(st, sizeof(st));
    CreateDirectoryW(g_dir, nullptr);
    wchar_t txt[MAX_PATH], dmp[MAX_PATH];
    _snwprintf_s(txt, MAX_PATH, _TRUNCATE, L"%scrash_%S.txt", g_dir, st);
    _snwprintf_s(dmp, MAX_PATH, _TRUNCATE, L"%scrash_%S.dmp", g_dir, st);
    wcscpy_s(dmp_path_out, MAX_PATH, dmp);

    Out o;
    o.h = CreateFileW(txt, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const EXCEPTION_RECORD* r = ep ? ep->ExceptionRecord : nullptr;
    const CONTEXT*          c = ep ? ep->ContextRecord : nullptr;
    const DWORD             code = r ? r->ExceptionCode : 0;

    o.put("Far Cry 5 VR mod: crash report\r\n");
    o.put("Mod build: %s%s (%s, linked %s)\r\n", FC5VR_GIT_HASH, FC5VR_GIT_DIRTY ? "-dirty" : "", FC5VR_GIT_BRANCH, g_link_time);
    o.put("Time: %s, %.1f s after the mod loaded\r\n", st, ms_since_load() / 1000.0);
    o.put("Exception: 0x%08lX %s\r\n", (unsigned long)code, code_name(code));
    if (r && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && r->NumberParameters >= 2) {
        const auto  kind = r->ExceptionInformation[0];
        const char* k    = kind == 0 ? "read from" : kind == 1 ? "write to" : kind == 8 ? "execute at (DEP)" : "access to";
        o.put("  %s address 0x%llx\r\n", k, (unsigned long long)r->ExceptionInformation[1]);
        if (code == EXCEPTION_IN_PAGE_ERROR && r->NumberParameters >= 3) o.put("  status 0x%08llx\r\n", (unsigned long long)r->ExceptionInformation[2]);
    }
    if (r && code == 0xE06D7363) {
        char type[128], what[256];
        describe_cpp(r, type, sizeof(type), what, sizeof(what));
        o.put("  type %s%s%s\r\n", type[0] ? type : "?", what[0] ? ", what(): " : "", what);
    }
    if (r && code == 0xC0000409 && r->NumberParameters >= 1) o.put("  fast fail code %llu\r\n", (unsigned long long)r->ExceptionInformation[0]);
    char at[MAX_PATH + 32];
    ModInfo m;
    const uintptr_t pc = r ? (uintptr_t)r->ExceptionAddress : 0;
    if (find_module(pc, m)) o.put("At: %s+0x%llx (%s)\r\n", m.name(), (unsigned long long)(pc - m.base), m.path);
    else o.put("At: 0x%llx (in no module)\r\n", (unsigned long long)pc);

    const int ts = last_step_of(tid);
    if (ts >= 0) o.put("Thread: %lu, its last start step: %d %s (+%u ms)\r\n", (unsigned long)tid, ts + 1, g_ring[ts % kRing].name, g_ring[ts % kRing].ms);
    else o.put("Thread: %lu (took no start step)\r\n", (unsigned long)tid);
    const uint32_t n = g_steps.load();
    if (n > 0) {
        const Step& s = g_ring[(n - 1) % kRing];
        o.put("Last start step: %u %s (+%u ms, thread %lu)%s\r\n", n, s.name, s.ms, (unsigned long)s.tid, g_vr_running.load() ? "; VR was running" : "; VR had not started");
    }
    o.put("\r\nLast %u start steps:\r\n", n < 30 ? n : 30);
    for (uint32_t k = n > 30 ? n - 30 : 0; k < n; ++k) put_step(o, k, g_ring[k % kRing]);

    if (c) {
        o.put("\r\nRegisters:\r\n");
        o.put("  RAX %016llx RBX %016llx RCX %016llx RDX %016llx\r\n", c->Rax, c->Rbx, c->Rcx, c->Rdx);
        o.put("  RSI %016llx RDI %016llx RBP %016llx RSP %016llx\r\n", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
        o.put("  R8  %016llx R9  %016llx R10 %016llx R11 %016llx\r\n", c->R8, c->R9, c->R10, c->R11);
        o.put("  R12 %016llx R13 %016llx R14 %016llx R15 %016llx\r\n", c->R12, c->R13, c->R14, c->R15);
        o.put("  RIP %016llx EFLAGS %08lx\r\n", c->Rip, (unsigned long)c->EFlags);

        DWORD64   pcs[64];
        const int depth = walk_stack(*c, pcs, 64);
        o.put("\r\nStack (unwound from the exception):\r\n");
        for (int i = 0; i < depth; ++i) {
            where((uintptr_t)pcs[i], at, sizeof(at));
            o.put("  #%-2d %s\r\n", i, at);
        }
        // Addresses inside modules found on the stack (helps where unwinding stops early).
        o.put("\r\nStack scan (values on the stack that point into modules, from RSP):\r\n");
        int shown = 0;
        for (int i = 0; i < 2048 && shown < 40; ++i) {
            DWORD64 v = 0;
            bool    ok = true;
            __try {
                v = reinterpret_cast<const DWORD64*>(c->Rsp)[i];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
            if (!ok) break;
            if (v < 0x10000) continue;
            ModInfo vm;
            if (find_module((uintptr_t)v, vm)) {
                o.put("  rsp+0x%-5x %s+0x%llx\r\n", i * 8, vm.name(), (unsigned long long)(v - vm.base));
                ++shown;
            }
        }
    }
    o.put("\r\nModules (not Windows'): base size timestamp path\r\n");
    put_modules(o);
    o.put("\r\nMinidump: %S\r\n", dmp);
    o.flush();

    // The dump (with the thread list, unloaded modules and the memory the stacks point to).
    HANDLE d = CreateFileW(dmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    BOOL   dumped = FALSE;
    DWORD  dump_err = 0;
    if (d != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{ tid, ep, FALSE };
        dumped   = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), d,
                                     (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory), ep ? &mei : nullptr,
                                     nullptr, nullptr);
        dump_err = dumped ? 0 : GetLastError();
        CloseHandle(d);
    } else {
        dump_err = GetLastError();
    }
    o.put("  %s (error %lu)\r\n", dumped ? "written" : "NOT written", (unsigned long)dump_err);
    o.flush();
    if (o.h != INVALID_HANDLE_VALUE) CloseHandle(o.h);
}

// One line straight into fc5vr_log.txt (works when spdlog is stuck).
void append_log_line(const char* text)
{
    if (!g_log_path[0]) return;
    HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, text, (DWORD)strlen(text), &w, nullptr);
    CloseHandle(h);
}

void flush_spdlog()
{
    try {
        if (auto l = spdlog::default_logger()) l->flush();
    } catch (...) {
    }
}

void report_now(EXCEPTION_POINTERS* ep, DWORD tid)
{
    wchar_t dmp[MAX_PATH]{};
    write_report(ep, tid, dmp);
    SetEvent(g_done_evt);
    char line[MAX_PATH + 200], st[32];
    SYSTEMTIME t{};
    GetLocalTime(&t);
    _snprintf_s(st, sizeof(st), _TRUNCATE, "%02u:%02u:%02u.%03u", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[%s] [E] [tid:%lu] [Crash] 0x%08lX %s: report and dump in %S (crash_*.txt / .dmp)\n", st, (unsigned long)tid,
                (unsigned long)code, code_name(code), g_dir);
    append_log_line(line);
    flush_spdlog();
    SetEvent(g_flush_evt);
}

DWORD WINAPI report_thread(LPVOID)
{
    g_helper_running = true;
    WaitForSingleObject(g_req_evt, INFINITE);
    report_now(g_ep, g_crash_tid);
    return 0;
}

// The report, once per process (a second crash meanwhile waits for the first report).
void report_once(EXCEPTION_POINTERS* ep)
{
    if (g_reporting.exchange(1) == 0) {
        g_ep        = ep;
        g_crash_tid = GetCurrentThreadId();
        if (g_helper_running.load()) {
            SetEvent(g_req_evt);
            WaitForSingleObject(g_done_evt, 90000);
            WaitForSingleObject(g_flush_evt, 3000);
        } else {
            report_now(ep, g_crash_tid);
        }
    } else {
        // Another thread crashed while the first report is written: let that one finish.
        WaitForSingleObject(g_done_evt, 90000);
    }
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    if (t_in_filter) return EXCEPTION_CONTINUE_SEARCH;   // a filter we chained to called us back
    t_in_filter = 1;
    report_once(ep);
    LONG                         r    = EXCEPTION_CONTINUE_SEARCH;
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_prev;
    if (prev && prev != &crash_filter) {
        __try {
            r = prev(ep);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            r = EXCEPTION_CONTINUE_SEARCH;
        }
    }
    t_in_filter = 0;
    // EXCEPTION_CONTINUE_SEARCH: Windows' error reporting runs (Application Error event 1000) and closes the game.
    return r;
}

// The last hardware exception each thread raised (first chance, before any handler): with /EHa a catch(...) swallows
// access violations, and fc5_crash_log_caught tells from this what was caught. Only noted here, never handled.
struct SehNote
{
    DWORD     code;
    uintptr_t addr;
    uintptr_t info[2];
    DWORD64   pcs[16];
    int       depth;
};
thread_local SehNote t_seh{};
std::atomic<PVOID>   g_veh{ nullptr };   // our vectored handler's registration (re-added at the front by reassert)

bool is_hardware_fault(DWORD c)
{
    return c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_IN_PAGE_ERROR || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_PRIV_INSTRUCTION ||
           c == EXCEPTION_INT_DIVIDE_BY_ZERO || c == EXCEPTION_STACK_OVERFLOW || c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
}

void report_once(EXCEPTION_POINTERS* ep);

LONG CALLBACK seh_note(EXCEPTION_POINTERS* ep)
{
    const DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_STACK_OVERFLOW && !t_in_filter && g_helper_running.load()) {
        // Reported now, before any handler runs: on the game's render thread the game's own exception filters run on
        // the exhausted stack, overflow again, and Windows ends the game with no report and no event (test 2026-10-23).
        // report_once only signals the report thread and waits (little stack).
        t_in_filter = 1;
        report_once(ep);
        t_in_filter = 0;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (is_hardware_fault(c)) {
        t_seh.code    = c;
        t_seh.addr    = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        t_seh.info[0] = ep->ExceptionRecord->NumberParameters >= 1 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
        t_seh.info[1] = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
        t_seh.depth   = c == EXCEPTION_STACK_OVERFLOW ? 0 : walk_stack(*ep->ContextRecord, t_seh.pcs, 16);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// terminate / pure call / abort: the report from here (a context captured now), then the CRT's own way out (abort ->
// fast fail -> Windows' error report). Not by raising an exception: the builds use /EHa, so a catch(...) on the way
// would swallow it and the game would go on after an abort.
__declspec(noinline) void report_fatal(DWORD code)
{
    EXCEPTION_RECORD er{};
    er.ExceptionCode    = code;
    er.ExceptionFlags   = EXCEPTION_NONCONTINUABLE;
    er.ExceptionAddress = _ReturnAddress();
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_POINTERS ep{ &er, &ctx };
    report_once(&ep);
}

[[noreturn]] void on_terminate()
{
    report_fatal(kCodeTerminate);
    std::abort();
}

void __cdecl on_purecall()
{
    report_fatal(kCodePurecall);
    std::abort();
}

void __cdecl on_sigabrt(int)
{
    report_fatal(kCodeAbort);
}

// ---- crash test ----

#pragma warning(push)
#pragma warning(disable : 4717)   // recursive on all paths: that is the point
__declspec(noinline) int overflow_stack(int depth)
{
    volatile char pad[4096];
    pad[0] = (char)depth;
    return overflow_stack(depth + 1) + pad[0];
}
#pragma warning(pop)

void read_crash_test()
{
    wchar_t p[MAX_PATH];
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%sfc5vr_crashtest.txt", g_exe_dir);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char  b[256]{};
    DWORD got = 0;
    ReadFile(h, b, sizeof(b) - 1, &got, nullptr);
    CloseHandle(h);
    // Deleted when the test fires, not here: the game's first process (Steam / Ubisoft Connect's, which relaunches the
    // game within seconds) loads the mod too and would use it up.
    b[got] = 0;
    for (char* c = b; *c; ++c)
        if (*c == '\r' || *c == '\n') { *c = 0; break; }
    char* s = b;
    while (*s == ' ') ++s;
    char* sp = strchr(s, ' ');
    if (!sp) return;
    *sp = 0;
    char* prefix = sp + 1;
    while (*prefix == ' ') ++prefix;
    strncpy_s(g_ct_kind, s, _TRUNCATE);
    strncpy_s(g_ct_prefix, prefix, _TRUNCATE);
    g_ct_armed = g_ct_prefix[0] != 0;
}

void run_crash_test(const char* step_name)
{
    wchar_t p[MAX_PATH];
    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%sfc5vr_crashtest.txt", g_exe_dir);
    DeleteFileW(p);
    if (g_log_ready.load()) {
        spdlog::warn("[Boot] crash test (fc5vr_crashtest.txt): {} at step {}", g_ct_kind, step_name);
        flush_spdlog();
    }
    const std::string_view k = g_ct_kind;
    if (k == "av") *reinterpret_cast<volatile int*>(0x10) = 1;
    else if (k == "stack") overflow_stack(0);
    else if (k == "abort") std::abort();
    else if (k == "fastfail") __fastfail(7 /* FAST_FAIL_FATAL_APP_EXIT */);
    else if (k == "throw") throw std::runtime_error("fc5vr crash test: an exception nobody catches");
    else if (k == "terminate") std::terminate();
    else if (k == "hang") Sleep(150000);
}

// ---- boot header helpers (normal code: the log is fine to use here) ----

std::string utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

std::wstring reg_str(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[2048]{};
    DWORD   size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

bool reg_dword(HKEY root, const wchar_t* key, const wchar_t* value, DWORD& out)
{
    DWORD size = sizeof(out);
    return RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
}

bool reg_key_exists(HKEY root, const wchar_t* key)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

std::string file_version(const std::wstring& path, std::string* product = nullptr)
{
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0) return "no version";
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return "no version";
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    std::string v = "no version";
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fi), &len) && fi)
        v = fmt::format("{}.{}.{}.{}", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    if (product) {
        struct Lang { WORD lang, cp; }* langs = nullptr;
        if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) && langs && len >= sizeof(Lang)) {
            for (const wchar_t* field : { L"ProductName", L"FileDescription" }) {
                wchar_t q[128];
                _snwprintf_s(q, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\%s", langs[0].lang, langs[0].cp, field);
                wchar_t* s = nullptr;
                if (VerQueryValueW(data.data(), q, reinterpret_cast<void**>(&s), &len) && s && len > 0) *product += utf8(s) + " ";
            }
        }
    }
    return v;
}

std::string read_text(const std::wstring& path)
{
    std::string s;
    HANDLE      h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return s;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        s.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ReadFile(h, s.data(), (DWORD)s.size(), &got, nullptr);
        s.resize(got);
    }
    CloseHandle(h);
    return s;
}

// The string value of the first "key": "value" in a JSON text (runtime and layer manifests are small and flat).
std::string json_str(const std::string& text, const char* key)
{
    const std::string k = std::string{ "\"" } + key + "\"";
    auto p = text.find(k);
    if (p == std::string::npos) return {};
    p = text.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = text.find('"', p);
    if (p == std::string::npos) return {};
    std::string v;
    for (size_t i = p + 1; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) ++i;
        v += text[i];
    }
    return v;
}

void log_runtime_manifest(const char* what, const std::wstring& json)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(json, ec)) {
        spdlog::warn("[Boot] {}: {} (the file does not exist: no OpenXR runtime this way)", what, utf8(json));
        return;
    }
    const auto text = read_text(json);
    const auto name = json_str(text, "name");
    const auto lib  = json_str(text, "library_path");
    fs::path   lib_path = fs::path(lib);
    if (!lib.empty() && lib_path.is_relative()) lib_path = fs::path(json).parent_path() / lib_path;
    const bool lib_ok = !lib.empty() && fs::exists(lib_path, ec);
    spdlog::info("[Boot] {}: {} (\"{}\"), library {} {}", what, utf8(json), name.empty() ? "no name" : name, lib_path.string(), lib_ok ? "exists" : "MISSING");
    if (!lib_ok) spdlog::warn("[Boot] {}: the runtime's DLL is missing: VR cannot start (reinstall or re-select the headset's OpenXR runtime)", what);
}

void log_layers(HKEY root, const char* root_name)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ, &k) != ERROR_SUCCESS) {
        spdlog::info("[Boot] OpenXR implicit layers ({}): none", root_name);
        return;
    }
    int n = 0;
    for (DWORD i = 0;; ++i) {
        wchar_t name[1024];
        DWORD   name_len = 1024, type = 0, data = 0, data_len = sizeof(data);
        if (RegEnumValueW(k, i, name, &name_len, nullptr, &type, reinterpret_cast<BYTE*>(&data), &data_len) != ERROR_SUCCESS) break;
        const auto text  = read_text(name);
        const auto lname = json_str(text, "name");
        spdlog::info("[Boot] OpenXR implicit layer ({}): {} \"{}\" {}{}", root_name, utf8(name), lname.empty() ? "?" : lname, data == 0 ? "ENABLED" : "disabled",
                     text.empty() ? " (manifest missing)" : "");
        ++n;
    }
    RegCloseKey(k);
    if (n == 0) spdlog::info("[Boot] OpenXR implicit layers ({}): none", root_name);
}

void log_gpus()
{
    const wchar_t* cls = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
    HKEY           k   = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, cls, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t sub[64];
        DWORD   sub_len = 64;
        if (RegEnumKeyExW(k, i, sub, &sub_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (sub[0] < L'0' || sub[0] > L'9') continue;
        const std::wstring key  = std::wstring{ cls } + L"\\" + sub;
        const auto         desc = reg_str(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDesc");
        if (desc.empty()) continue;
        const auto ver  = reg_str(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverVersion");
        const auto date = reg_str(HKEY_LOCAL_MACHINE, key.c_str(), L"DriverDate");
        uint64_t   vram = 0;
        DWORD      size = sizeof(vram);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"HardwareInformation.qwMemorySize", RRF_RT_ANY, nullptr, &vram, &size) != ERROR_SUCCESS) {
            DWORD v32 = 0;
            size      = sizeof(v32);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"HardwareInformation.MemorySize", RRF_RT_ANY, nullptr, &v32, &size) == ERROR_SUCCESS) vram = v32;
        }
        spdlog::info("[Boot] GPU {}: {}, driver {} ({}), {} MB", utf8(sub), utf8(desc), utf8(ver), utf8(date), vram / (1024 * 1024));
    }
    RegCloseKey(k);
}

void log_module_version(const wchar_t* name)
{
    HMODULE m = GetModuleHandleW(name);
    if (!m) {
        spdlog::info("[Boot] {}: not loaded", utf8(name));
        return;
    }
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(m, p, MAX_PATH);
    spdlog::info("[Boot] {}: {} {}", utf8(name), utf8(p), file_version(p));
}

void log_bin_dlls()
{
    WIN32_FIND_DATAW fd{};
    HANDLE           f = FindFirstFileW((std::wstring{ g_exe_dir } + L"*.dll").c_str(), &fd);
    std::string      list;
    std::vector<std::string> warnings;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            std::wstring low  = name;
            for (auto& ch : low) ch = (wchar_t)towlower(ch);
            std::string product;
            const auto  ver = file_version(std::wstring{ g_exe_dir } + name, &product);
            list += fmt::format(" {} ({} KB, {})", utf8(name), (((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow) / 1024, ver);
            std::string plow = product;
            for (auto& ch : plow) ch = (char)tolower((unsigned char)ch);
            if (low == L"openvr_api.dll") warnings.push_back("openvr_api.dll is in bin: the mod tries OpenVR first; remove it to use OpenXR");
            else if (low == L"d3d11.dll" || low == L"dinput8.dll" || low == L"version.dll" || low == L"winmm.dll" || low == L"d3d9.dll" || low == L"dxgi_.dll")
                warnings.push_back(utf8(name) + " is in bin: another mod or overlay loads itself into the game this way and may clash with the VR mod");
            if (plow.find("reshade") != std::string::npos) warnings.push_back(utf8(name) + " is ReShade: it can clash with the VR mod");
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    spdlog::info("[Boot] DLLs in bin:{}", list.empty() ? " none" : list);
    std::error_code ec;
    for (const wchar_t* rs : { L"ReShade.ini", L"ReShade.log", L"reshade-shaders" })
        if (std::filesystem::exists(std::wstring{ g_exe_dir } + rs, ec)) warnings.push_back(utf8(rs) + " is in bin: ReShade is (or was) installed there");
    for (const auto& w : warnings) spdlog::warn("[Boot] {}", w);
}

// Stacks of other (live) threads for the start watchdog: the thread is suspended only while its context is taken and
// walked (no heap use meanwhile: it may hold the heap's lock).
void log_thread_stack(DWORD tid, const char* why)
{
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) {
        spdlog::info("[Boot]   thread {} ({}): gone", tid, why);
        return;
    }
    DWORD exit_code = 0;
    if (GetExitCodeThread(th, &exit_code) && exit_code != STILL_ACTIVE) {
        CloseHandle(th);
        spdlog::info("[Boot]   thread {} ({}): ended", tid, why);
        return;
    }
    DWORD64 pcs[48];
    int     depth = 0;
    if (tid != GetCurrentThreadId() && SuspendThread(th) != (DWORD)-1) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &c)) depth = walk_stack(c, pcs, 48);
        ResumeThread(th);
    }
    CloseHandle(th);
    spdlog::info("[Boot]   thread {} ({}): {} frames", tid, why, depth);
    for (int i = 0; i < depth; ++i) {
        char at[MAX_PATH + 32];
        where((uintptr_t)pcs[i], at, sizeof(at));
        spdlog::info("[Boot]     #{:<2} {}", i, at);
    }
}

void log_modules_spdlog()
{
    std::vector<std::string> lines;
    for_each_module([&](uintptr_t base, uint32_t size, const wchar_t* path, size_t len) {
        if (!is_windows_path(path, len)) lines.push_back(fmt::format("{:016x} {:08x} {:08x} {}", base, size, pe_timestamp(base), utf8(std::wstring(path, len))));
        return false;
    });
    spdlog::info("[Boot] modules (not Windows'): base size timestamp path");
    for (const auto& l : lines) spdlog::info("[Boot]   {}", l);
}

void stuck_report(int secs)
{
    const uint32_t n = g_steps.load();
    if (n == 0) return;
    const Step& s = g_ring[(n - 1) % kRing];
    spdlog::warn("[Boot] no new start step for {} s: still at step {} {} (thread {}); VR has not started", secs, n, s.name, s.tid);
    // The threads that took the last steps, newest first (at most 4).
    DWORD tids[4]{};
    int   count = 0;
    for (uint32_t k = 0; k < 12 && k < n && count < 4; ++k) {
        const Step& t = g_ring[(n - 1 - k) % kRing];
        bool        seen = false;
        for (int j = 0; j < count; ++j) seen |= tids[j] == t.tid;
        if (!seen) {
            tids[count++] = t.tid;
            log_thread_stack(t.tid, t.name);
        }
    }
    log_modules_spdlog();
    flush_spdlog();
}
}   // namespace

// ---- vrframework's entry points (Fc5Boot.hpp) ----

void fc5_boot_step(const char* name) { fc5::crash::step(name); }

void fc5_boot_result(const char* name, long result)
{
    const uint32_t n   = g_steps.load();
    const DWORD    tid = GetCurrentThreadId();
    for (uint32_t k = 0; k < kRing && k < n; ++k) {
        Step& s = g_ring[(n - 1 - k) % kRing];
        if (s.tid == tid && s.name == name) {
            s.result     = result;
            s.has_result = true;
            if (g_log_ready.load()) spdlog::info("[Boot] step {} {}: result {} (0x{:08x}, +{} ms)", n - k, name, result, (uint32_t)result, ms_since_load());
            return;
        }
    }
}

void fc5_boot_vr_running()
{
    if (g_vr_running.load()) return;
    fc5::crash::step("VR running");
    if (g_vr_running.exchange(true)) return;
    spdlog::info("[Boot] VR running: first frame in the headset ({:.1f} s after load)", ms_since_load() / 1000.0);
    flush_spdlog();
}

void fc5_crash_reassert(const char* at_step)
{
    // Our vectored handler back to the front of the list: the game adds its own later (a stack overflow on its render
    // thread went to the game's handler, Ubisoft's crash reporter and exit code 3, before ours: test 2026-10-23 06:05).
    if (PVOID front = AddVectoredExceptionHandler(1, &seh_note)) {
        if (PVOID old = g_veh.exchange(front)) RemoveVectoredExceptionHandler(old);
    }
    const LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(&crash_filter);
    if (cur == &crash_filter) return;
    char        text[160];
    char        mod[MAX_PATH + 32] = "none";
    if (cur) where((uintptr_t)cur, mod, sizeof(mod));
    if (cur && cur != g_prev) {
        g_prev = cur;
        _snprintf_s(text, sizeof(text), _TRUNCATE, "[Crash] at %s the crash filter had been replaced by %s: ours put back first, chained to it", at_step, mod);
    } else {
        _snprintf_s(text, sizeof(text), _TRUNCATE, "[Crash] at %s the crash filter had been removed (%s): ours put back", at_step, mod);
    }
    if (g_log_ready.load()) spdlog::warn("{}", text);
    else early_note(text);
}

void fc5_crash_log_caught(const char* at)
{
    const SehNote n = t_seh;
    t_seh.code      = 0;
    if (n.code == 0) {
        spdlog::error("[Crash] {}: a C++ exception was caught there (no access violation on this thread)", at);
        return;
    }
    char where_at[MAX_PATH + 32];
    where(n.addr, where_at, sizeof(where_at));
    spdlog::error("[Crash] {}: caught 0x{:08X} {} at {}{}", at, n.code, code_name(n.code), where_at,
                  n.code == EXCEPTION_ACCESS_VIOLATION ? fmt::format(" ({} address 0x{:x})", n.info[0] == 0 ? "read from" : n.info[0] == 1 ? "write to" : "execute at", n.info[1])
                                                       : std::string{});
    for (int i = 0; i < n.depth; ++i) {
        where((uintptr_t)n.pcs[i], where_at, sizeof(where_at));
        spdlog::error("[Crash]   #{:<2} {}", i, where_at);
    }
    flush_spdlog();
}

namespace fc5::crash {

void step(const char* name)
{
    const uint32_t i = g_steps.fetch_add(1);
    g_ring[i % kRing] = Step{ ms_since_load(), GetCurrentThreadId(), name, 0, false };
    g_last_step_tick  = GetTickCount64();
    if (g_log_ready.load()) spdlog::info("[Boot] step {} {} (+{} ms, thread {})", i + 1, name, g_ring[i % kRing].ms, GetCurrentThreadId());
    fc5_crash_reassert(name);
    if (g_ct_armed.load() && strncmp(name, g_ct_prefix, strlen(g_ct_prefix)) == 0 && g_ct_armed.exchange(false)) run_crash_test(name);
}

void install_early(HMODULE self)
{
    g_self      = self;
    g_load_tick = GetTickCount64();
    const DWORD n = GetModuleFileNameW(nullptr, g_exe_dir, MAX_PATH);
    if (wchar_t* cut = wcsrchr(g_exe_dir, L'\\'); cut && n > 0) cut[1] = 0;
    _snwprintf_s(g_dir, MAX_PATH, _TRUNCATE, L"%sfc5vr_logs\\", g_exe_dir);
    const UINT wl = GetWindowsDirectoryW(g_windir, MAX_PATH - 1);
    if (wl > 0 && wl < MAX_PATH - 1) {
        if (g_windir[wl - 1] != L'\\') wcscat_s(g_windir, L"\\");
        g_windir_len = wcslen(g_windir);
    }
    read_crash_test();
    if (const uint32_t linked = pe_timestamp((uintptr_t)self)) {
        // Seconds since 1970 (UTC) -> FILETIME -> local time.
        ULARGE_INTEGER u{};
        u.QuadPart = (uint64_t)linked * 10000000ull + 116444736000000000ull;
        FILETIME   ft{ u.LowPart, u.HighPart }, local{};
        SYSTEMTIME st{};
        if (FileTimeToLocalFileTime(&ft, &local) && FileTimeToSystemTime(&local, &st))
            _snprintf_s(g_link_time, sizeof(g_link_time), _TRUNCATE, "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    }

    g_req_evt   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_done_evt  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_flush_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // Its own stack, big enough for the report (it starts once the loader lets threads run).
    if (HANDLE t = CreateThread(nullptr, 256 * 1024, &report_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)) CloseHandle(t);

    g_prev = SetUnhandledExceptionFilter(&crash_filter);
    g_veh = AddVectoredExceptionHandler(1, &seh_note);
    if (!std::get_terminate()) std::set_terminate(&on_terminate);
    if (!_get_purecall_handler()) _set_purecall_handler(&on_purecall);
    if (auto old = signal(SIGABRT, &on_sigabrt); old != SIG_DFL && old != SIG_ERR) signal(SIGABRT, old);   // someone else's: keep it

    step("dll loaded");
}

void log_ready(const std::wstring& log_path)
{
    namespace fs = std::filesystem;
    wcsncpy_s(g_log_path, log_path.c_str(), _TRUNCATE);
    const auto dir = (fs::path(log_path).parent_path() / "fc5vr_logs").wstring() + L"\\";
    wcsncpy_s(g_dir, dir.c_str(), _TRUNCATE);
    g_log_ready         = true;
    const uint32_t done = g_steps.load();
    for (uint32_t i = 0; i < done && i < kRing; ++i) {
        const Step& s = g_ring[i];
        spdlog::info("[Boot] step {} {} (+{} ms, thread {}){}", i + 1, s.name, s.ms, s.tid, " (before the log was open)");
    }
    for (int i = 0; i < g_early_note_count.load() && i < 4; ++i) spdlog::warn("{}", g_early_notes[i]);
    if (g_ct_prefix[0]) spdlog::warn("[Boot] crash test armed (fc5vr_crashtest.txt, deleted when it fires): {} at the step starting with \"{}\"", g_ct_kind, g_ct_prefix);
}

void cleanup_old_reports()
{
    namespace fs = std::filesystem;
    std::error_code              ec;
    std::vector<fs::path>        txt, dmp;
    for (const auto& e : fs::directory_iterator(fs::path(g_dir), ec)) {
        const auto name = e.path().filename().wstring();
        if (name.rfind(L"crash_", 0) != 0) continue;
        if (e.path().extension() == L".txt") txt.push_back(e.path());
        if (e.path().extension() == L".dmp") dmp.push_back(e.path());
    }
    auto keep = [&](std::vector<fs::path>& v, size_t n) {
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.filename() > b.filename(); });
        for (size_t i = n; i < v.size(); ++i) fs::remove(v[i], ec);
    };
    keep(txt, 5);
    keep(dmp, 3);
}

void log_boot_header()
{
    try {
        wchar_t self_path[MAX_PATH]{};
        GetModuleFileNameW(g_self, self_path, MAX_PATH);
        spdlog::info("[Boot] mod build {}{} ({}, linked {}), {}", FC5VR_GIT_HASH, FC5VR_GIT_DIRTY ? "-dirty" : "", FC5VR_GIT_BRANCH, g_link_time, utf8(self_path));

        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::error_code ec;
        const auto      exe_size = std::filesystem::file_size(exe, ec);
        spdlog::info("[Boot] game {} ({} bytes, version {})", utf8(exe), exe_size, file_version(exe));
        wchar_t cwd[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        spdlog::info("[Boot] working folder {}", utf8(cwd));

        const wchar_t* nt   = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        DWORD          ubr  = 0;
        reg_dword(HKEY_LOCAL_MACHINE, nt, L"UBR", ubr);
        spdlog::info("[Boot] Windows {} {} build {}.{}", utf8(reg_str(HKEY_LOCAL_MACHINE, nt, L"ProductName")), utf8(reg_str(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion")),
                     utf8(reg_str(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild")), ubr);
        log_gpus();

        const auto active = reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
        if (active.empty()) spdlog::warn("[Boot] no OpenXR runtime is set as the system's (HKLM Khronos ActiveRuntime): set the headset app's OpenXR runtime as the default");
        else log_runtime_manifest("system OpenXR runtime", active);
        wchar_t env[2048]{};
        if (GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env, 2048) > 0) log_runtime_manifest("XR_RUNTIME_JSON (used instead)", env);
        log_layers(HKEY_LOCAL_MACHINE, "HKLM");
        log_layers(HKEY_CURRENT_USER, "HKCU");

        log_bin_dlls();
        log_module_version(L"msvcp140.dll");
        log_module_version(L"vcruntime140.dll");
        const auto vc = reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64", L"Version");
        spdlog::info("[Boot] VC++ runtime installed (x64): {}", vc.empty() ? std::string{ "not registered" } : utf8(vc));
        const bool vigem = reg_key_exists(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\ViGEmBus");
        if (vigem) spdlog::info("[Boot] ViGEmBus driver: installed");
        else spdlog::warn("[Boot] ViGEmBus driver: NOT installed (the motion controllers drive the game through it)");
    } catch (const std::exception& e) {
        spdlog::error("[Boot] header: {}", e.what());
    }
}

void start_boot_watchdog()
{
    std::thread([] {
        uint32_t seen  = ~0u;
        int      stage = 0;
        uint64_t last_reassert = GetTickCount64();
        for (;;) {
            Sleep(1000);
            const uint64_t now = GetTickCount64();
            if (now - last_reassert > 5000) {
                last_reassert = now;
                fc5_crash_reassert("watchdog");   // someone (the game, an overlay) may take the filter later
            }
            if (g_vr_running.load()) continue;    // after the success line the 6 s present watchdog takes over
            const uint32_t n = g_steps.load();
            if (n != seen) {
                seen  = n;
                stage = 0;
            }
            const uint64_t idle = now - g_last_step_tick.load();
            if (stage == 0 && idle >= 30000) {
                stage = 1;
                stuck_report(30);
            } else if (stage == 1 && idle >= 120000) {
                stage = 2;
                stuck_report(120);
            }
        }
    }).detach();
}

void process_exit(bool normal)
{
    // DllMain (loader lock held, other threads gone): no spdlog, no heap, one plain line. A process killed with TerminateProcess
    // never gets here, so after a normal quit this is the log's last line and its absence means killed (or a crash, which
    // has its own report). Homard 27 Oct: two logs that "ended mid-play" were plain process ends, but nothing said so.
    char       line[160];
    SYSTEMTIME t{};
    GetLocalTime(&t);
    sprintf_s(line, "[%02u:%02u:%02u.%03u] [I] [tid:%lu] [Boot] process exit (%s); lines of the last second may be missing above\r\n", t.wHour, t.wMinute, t.wSecond,
              t.wMilliseconds, GetCurrentThreadId(), normal ? "the game quit: ExitProcess" : "the mod's DLL unloaded");
    append_log_line(line);
}

}   // namespace fc5::crash
