#pragma once
#ifdef USE_DXGI_HOOK
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <dxgi.h>
//#include <string>

// Inline to avoid ODR violations when this header is included in multiple TUs
inline HMODULE g_dxgi = nullptr;

// Inline to avoid multiple definitions across translation units
inline bool load_dxgi()
{
    if (g_dxgi) {
        return true;
    }

    wchar_t buffer[MAX_PATH]{ 0 };
    if (GetSystemDirectoryW(buffer, MAX_PATH) != 0) {
        // Load the original dinput8.dll
        if ((g_dxgi = LoadLibraryW((std::wstring{ buffer } + L"\\dxgi.dll").c_str())) == NULL) {
            return false;
        }

        return true;
    }
    return false;
}
inline HMODULE get_dxgi()
{
    if (!g_dxgi) {
        load_dxgi();
    }
    return g_dxgi;
}

//#pragma comment(linker, "/EXPORT:CompatValue=dxgi.CompatValue,DATA")
//#pragma comment(linker, "/EXPORT:CompatString=dxgi.CompatString,DATA")
// --- Main Function Exports ---
//#pragma comment(linker, "/EXPORT:CreateDXGIFactory=dxgi.CreateDXGIFactory")
//#pragma comment(linker, "/EXPORT:CreateDXGIFactory1=dxgi.CreateDXGIFactory1")
//#pragma comment(linker, "/EXPORT:CreateDXGIFactory2=dxgi.CreateDXGIFactory2")
//#pragma comment(linker, "/EXPORT:DXGID3D10CreateDevice=dxgi.DXGID3D10CreateDevice")
//#pragma comment(linker, "/EXPORT:DXGID3D10CreateLayeredDevice=dxgi.DXGID3D10CreateLayeredDevice")
//#pragma comment(linker, "/EXPORT:DXGID3D10GetImageBlockSize=dxgi.DXGID3D10GetImageBlockSize")
//#pragma comment(linker, "/EXPORT:DXGID3D10RegisterLayers=dxgi.DXGID3D10RegisterLayers")
//#pragma comment(linker, "/EXPORT:DXGIDeclareAdapterRemovalSupport=dxgi.DXGIDeclareAdapterRemovalSupport")
//#pragma comment(linker, "/EXPORT:DXGIGetDebugInterface=dxgi.DXGIGetDebugInterface")
//#pragma comment(linker, "/EXPORT:DXGIGetDebugInterface1=dxgi.DXGIGetDebugInterface1")
//#pragma comment(linker, "/EXPORT:DXGIReportAdapterConfiguration=dxgi.DXGIReportAdapterConfiguration")

// --- Other/Legacy Function Exports ---
//#pragma comment(linker, "/EXPORT:ApplyCompatResolution=dxgi.ApplyCompatResolution")
//#pragma comment(linker, "/EXPORT:DXGICancelPresents=dxgi.DXGICancelPresents")
//#pragma comment(linker, "/EXPORT:DXGIRevertToSxS=dxgi.DXGIRevertToSxS")
//#pragma comment(linker, "/EXPORT:DXGISetAppCompatStringPointer=dxgi.DXGISetAppCompatStringPointer")
//#pragma comment(linker, "/EXPORT:OpenAdapter10=dxgi.OpenAdapter10")
//#pragma comment(linker, "/EXPORT:OpenAdapter10_2=dxgi.OpenAdapter10_2")
//#pragma comment(linker, "/EXPORT:SetAppCompatStringPointer=dxgi.SetAppCompatStringPointer")
//

#define IMPLEMENT_PROXY_FORWARD(originalFunctionName, ...) \
    spdlog::info("{} proxy called", #originalFunctionName);\
    static auto dxgi = get_dxgi();                                                       \
    if(!dxgi) return E_FAIL; \
    using FnT = decltype(&originalFunctionName); \
    static auto original_fn = (FnT)GetProcAddress(dxgi, #originalFunctionName); \
    if (!original_fn) return E_FAIL; \
    return original_fn(__VA_ARGS__);

#pragma comment(linker, "/EXPORT:CreateDXGIFactory=PX_CreateDXGIFactory")
extern "C" __declspec(dllexport) HRESULT WINAPI PX_CreateDXGIFactory(REFIID riid, _COM_Outptr_ void** ppFactory)
{
    IMPLEMENT_PROXY_FORWARD(CreateDXGIFactory, riid, ppFactory);
}


#pragma comment(linker, "/EXPORT:CreateDXGIFactory1=PX_CreateDXGIFactory1")
extern "C" __declspec(dllexport) HRESULT WINAPI PX_CreateDXGIFactory1(REFIID riid, _COM_Outptr_ void** ppFactory)
{
    IMPLEMENT_PROXY_FORWARD(CreateDXGIFactory1, riid, ppFactory);
}


#pragma comment(linker, "/EXPORT:CreateDXGIFactory2=PX_CreateDXGIFactory2")
extern "C" __declspec(dllexport) HRESULT WINAPI PX_CreateDXGIFactory2(UINT Flags, REFIID riid, _COM_Outptr_ void **ppFactory)
{
    IMPLEMENT_PROXY_FORWARD(CreateDXGIFactory2, Flags, riid, ppFactory);
}

#pragma comment(linker, "/EXPORT:DXGIGetDebugInterface1=PX_DXGIGetDebugInterface1")
extern "C" __declspec(dllexport) HRESULT WINAPI PX_DXGIGetDebugInterface1(UINT Flags, REFIID riid, _COM_Outptr_ void **pDebug)
{
    IMPLEMENT_PROXY_FORWARD(DXGIGetDebugInterface1, Flags, riid, pDebug);
}

// All other exports are forwarded directly above; keep one example wrapper below.
#endif
