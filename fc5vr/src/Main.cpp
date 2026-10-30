#include "ModConfig.h"
#include "FarCry5/FarCry5Stereo.h"
#include "FarCry5/FarCry5Afw.h"
#include "ModPath.h"
#include "CrashLog.h"
#include "Framework.hpp"
#include "dgxiProxy.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <d3d12.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

// vrframework's release builds log to a null sink, so no log file is written.
// Replace the default logger with a real file (vr_log.txt, in the same folder
// as vr_config.txt: the Starfield folder, or %APPDATA%\VR\Starfield when the
// game folder isn't writable) so testers can send it. Overwritten each launch, after the previous one is
// kept in fc5vr_logs\ (newest 10, with Virtual Desktop's OpenXR.log of that run beside it).
// Returns true when this run continues the current log instead: the game's first process (started by Steam or Ubisoft
// Connect) loads the mod too and relaunches the game within seconds; since the log opens first thing, rotating there
// would keep that stub's few lines as "the previous run" (two of the ten slots per launch, 2026-10-23).
static bool ContinuesRelaunchedLog(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || fad.nFileSizeHigh != 0 || fad.nFileSizeLow > 256 * 1024) {
        return false;
    }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto t = [](const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    if (t(now) - t(fad.ftLastWriteTime) > 60ull * 10000000ull) {
        return false;   // older than a minute: a run of its own
    }
    std::ifstream f{ path, std::ios::binary };
    std::string   text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // That start never presented: the stub process (or a crash in the first seconds, kept in the same file).
    return text.find("[Boot] step") != std::string::npos && text.find("first present") == std::string::npos;
}

static bool KeepPreviousLog(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec) || fs::file_size(path, ec) == 0) {
        return false;
    }
    if (ContinuesRelaunchedLog(path)) {
        return true;
    }
    const auto dir = path.parent_path() / "fc5vr_logs";
    fs::create_directories(dir, ec);
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    SYSTEMTIME st{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        FILETIME local{};
        FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &st);
    }
    char stamp[32]{};
    sprintf_s(stamp, "%04u-%02u-%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fs::copy_file(path, dir / (std::string("fc5vr_log_") + stamp + ".txt"), fs::copy_options::overwrite_existing, ec);
    // VDXR rewrites this file when the next OpenXR instance starts, so now it still describes the previous run.
    const fs::path vdxr = "C:\\ProgramData\\Virtual Desktop\\OpenXR.log";
    if (fs::exists(vdxr, ec)) {
        fs::copy_file(vdxr, dir / (std::string("fc5vr_log_") + stamp + "_vdxr.txt"), fs::copy_options::overwrite_existing, ec);
    }
    std::vector<fs::directory_entry> logs;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const auto name = e.path().filename().string();
        if (name.rfind("fc5vr_log_", 0) == 0 && name.find("_vdxr") == std::string::npos) {
            logs.push_back(e);
        }
    }
    std::sort(logs.begin(), logs.end(), [](const auto& a, const auto& b) { return a.path().filename() > b.path().filename(); });
    for (size_t i = 10; i < logs.size(); ++i) {
        auto vd = logs[i].path();
        vd.replace_filename(vd.stem().string() + "_vdxr.txt");
        fs::remove(logs[i].path(), ec);
        fs::remove(vd, ec);
    }
    return false;
}

static constexpr const char* kLogPattern = "[%H:%M:%S.%e] [%L] [tid:%t] %v";

// The first thing the init thread does (before the early hooks, the headset check and the framework), so a crash in
// the first seconds leaves this run's log, not the previous one's.
void InstallFileLogger() {
    try {
        const auto path = Framework::get_persistent_dir("fc5vr_log.txt");
        bool       relaunch = false;
        try {
            relaunch = KeepPreviousLog(path);
        } catch (...) {
        }
        auto logger = spdlog::basic_logger_mt("fc5vr", path.string(), !relaunch);
        logger->set_pattern(kLogPattern);
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(logger);
        if (relaunch) {
            spdlog::info("---- the game started again (process {}): the lines above are the process that relaunched it ----", GetCurrentProcessId());
        }
        spdlog::info("fc5vr log started, writing to {}", path.string());
        fc5::crash::log_ready(path.wstring());
    } catch (const std::exception& e) {
        OutputDebugStringA(e.what()); // keep the null logger
    }
}

// Framework's constructor makes a null default logger and sets its pattern on every logger: ours back on top.
static void RestoreFileLogger() {
    if (auto logger = spdlog::get("fc5vr")) {
        spdlog::set_default_logger(logger);
        logger->set_pattern(kLogPattern);
        logger->flush_on(spdlog::level::info);
    }
}

bool verifyLeftHandedCoordinates() {
    // Create forward vector (in left-handed, +Z is forward)
    glm::vec3 forward(0.0f, 0.0f, 1.0f);

    // Create test rotation 90 degrees around Y axis
    glm::mat4 rotationY = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    // Apply rotation
    glm::vec3 result = glm::vec3(rotationY * glm::vec4(forward, 0.0f));

    // In left-handed: rotating +Z 90° around Y should give +X
    // In right-handed: rotating +Z 90° around Y should give -X
    bool isLeftHanded = glm::abs(result.x - 1.0f) < 0.00001f;

    return isLeftHanded;
}


// Research aid: with sfvr_dred.txt in the game folder, D3D12 Device Removed Extended Data
// (breadcrumbs + page faults) is turned on before the game creates its device, so a GPU fault
// can be explained in the log (NativeStereo reports it).
void EnableDredIfRequested() {
    if (GetFileAttributesA(fc5::mod_file("sfvr_dred.txt").c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    using get_debug_interface_t = HRESULT(WINAPI*)(REFIID, void**);
    const auto d3d12 = LoadLibraryA("d3d12.dll");
    const auto get_debug_interface = d3d12 ? reinterpret_cast<get_debug_interface_t>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
    ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
    if (get_debug_interface && SUCCEEDED(get_debug_interface(IID_PPV_ARGS(&settings))) && settings) {
        settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings->Release();
        OutputDebugStringA("[sfvr] DRED enabled\n");
    }
}

// Test runtime for this game only (Meta XR Simulator): bin/fc5vr_xr_runtime.txt holds the path of an
// OpenXR runtime manifest; the loader reads XR_RUNTIME_JSON before the system's active runtime.
static void UseTestRuntimeIfRequested() {
    FILE* f = nullptr;
    if (fopen_s(&f, fc5::mod_file("fc5vr_xr_runtime.txt").c_str(), "r") != 0 || f == nullptr) {
        return;
    }
    char path[1024]{};
    if (fgets(path, sizeof(path), f)) {
        for (char* c = path; *c; ++c) {
            if (*c == '\r' || *c == '\n') { *c = 0; break; }
        }
        if (path[0]) {
            SetEnvironmentVariableA("XR_RUNTIME_JSON", path);
            OutputDebugStringA("[fc5vr] XR_RUNTIME_JSON set from fc5vr_xr_runtime.txt\n");
        }
    }
    fclose(f);
}

// The launcher (FC5VR_Launcher.exe) writes bin/fc5vr_launcher.txt: "mode vr" or "mode flat", and
// "headset quest3". No file: VR, as before the launcher. The resolution multiplier is the resscale line
// in fc5vr_settings.txt, the same one F11 changes.
struct LauncherChoice {
    bool        flat = false;
    std::string headset;
};

static LauncherChoice ReadLauncherChoice() {
    LauncherChoice c;
    FILE*          f = nullptr;
    if (fopen_s(&f, fc5::mod_file("fc5vr_launcher.txt").c_str(), "r") != 0 || f == nullptr) {
        return c;
    }
    char line[256]{};
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n' || l.back() == ' ')) l.pop_back();
        if (l.rfind("mode ", 0) == 0) c.flat = l.substr(5) == "flat";
        if (l.rfind("headset ", 0) == 0) c.headset = l.substr(8);
    }
    fclose(f);
    return c;
}

void InitThread(HINSTANCE hModule) {
    fc5::crash::step("init thread");
    const LauncherChoice launcher = ReadLauncherChoice();
    if (launcher.flat) {
        // Flatscreen: the mod stays out of the way (no hooks, no OpenXR); dxgi.dll only forwards to the system's.
        try {
            KeepPreviousLog(fc5::mod_file("fc5vr_log.txt"));
            auto logger = spdlog::basic_logger_mt("fc5vr", fc5::mod_file("fc5vr_log.txt"), true);
            logger->set_pattern("[%H:%M:%S.%e] [%L] [tid:%t] %v");
            logger->info("[Launcher] mode flat (fc5vr_launcher.txt): the mod is off this run, the game runs on the monitor as without the mod");
            logger->flush();
        } catch (...) {
        }
        return;
    }
    InstallFileLogger();
    fc5::crash::step("log open");
    fc5::crash::cleanup_old_reports();
    EnableDredIfRequested();
    UseTestRuntimeIfRequested();
    fc5::crash::log_boot_header();
    fc5::crash::start_boot_watchdog();
    // Before the game makes its D3D11 device: the shader catalogue needs every shader it creates.
    fc5::crash::step("early hooks");
    FarCry5Stereo::install_early_hooks();
    Sleep(5000);
    fc5::crash::step("headset check");
    FarCry5Stereo::wait_headset_check();
    fc5::crash::step("framework");
    g_framework = std::make_unique<Framework>(hModule);
    RestoreFileLogger();
    fc5::crash::step("framework done");
    // AFW shaders compiled while the game boots (they cost the first frame ~4 s when compiled there).
    fc5::afw::precompile();
    FarCry5Stereo::log_start_notes();
    {
        char rt[1024]{};
        if (GetEnvironmentVariableA("XR_RUNTIME_JSON", rt, sizeof(rt)) > 0) spdlog::info("[XR] OpenXR runtime for this session: {} (XR_RUNTIME_JSON)", rt);
    }
    spdlog::info("[Launcher] mode vr, headset {}", launcher.headset.empty() ? std::string{ "not chosen (no fc5vr_launcher.txt)" } : launcher.headset);
#ifdef GLM_FORCE_LEFT_HANDED
    spdlog::info("GLM_FORCE_LEFT_HANDED is defined function={} clip={}", verifyLeftHandedCoordinates(), GLM_CONFIG_CLIP_CONTROL);
    assert(verifyLeftHandedCoordinates());
#else
    spdlog::info("GLM_FORCE_LEFT_HANDED is not defined");
#endif

}

BOOL APIENTRY DllMain( HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        // Crash filter, CRT handlers and the report thread first: a crash in the first seconds gets a report too.
        fc5::crash::install_early(hModule);
//        do {
//            Sleep(250);
//        } while (!IsDebuggerPresent());
//        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitThread, NULL, 0, NULL);
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        fc5::crash::process_exit(lpReserved != nullptr);
        break;
    }
    return TRUE;
}
