#pragma once

HMODULE g_dinput = 0;

bool load_dinput8()
{
    if (g_dinput) {
        return true;
    }

    wchar_t buffer[MAX_PATH]{ 0 };
    if (GetSystemDirectoryW(buffer, MAX_PATH) != 0) {
        // Load the original dinput8.dll
        if ((g_dinput = LoadLibraryW((std::wstring{ buffer } + L"\\dinput8.dll").c_str())) == NULL) {
            spdlog::error("Failed to load dinput8.dll");
            return false;
        }

        return true;
    }
    return false;
}

#pragma comment(linker, "/EXPORT:DirectInput8Create=direct_input8_create")

extern "C" {
    // DirectInput8Create wrapper for dinput8.dll
    __declspec(dllexport) HRESULT WINAPI
    direct_input8_create(HINSTANCE hinst, DWORD dw_version, REFIID riidltf, LPVOID* ppv_out, LPUNKNOWN punk_outer) {
        load_dinput8();
        typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
        auto original_DirectInput8Create = (DirectInput8Create_t)GetProcAddress(g_dinput, "DirectInput8Create");
        if (!original_DirectInput8Create) {
            // Handle the error, possibly by returning an error code
            return E_FAIL;
        }
        return original_DirectInput8Create(hinst, dw_version, riidltf, ppv_out, punk_outer);
    }
}
