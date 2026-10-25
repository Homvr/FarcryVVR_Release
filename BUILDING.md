# Building the Far Cry 5 VR mod (alpha 0.12.1, commit 4442a1a)

The build makes `dxgi.dll` (the mod) and `FC5VR_Launcher.exe`. Windows 10 or 11, 64-bit.

## What you need

1. **Visual Studio 2022 Build Tools** (or Visual Studio 2022) with the workload **Desktop development with C++**,
   including **C++ CMake tools for Windows** (it brings CMake and Ninja) and a Windows 10/11 SDK.
2. **Git** on the PATH: the first build downloads the open-source dependencies from GitHub at pinned commits
   (OpenXR, Dear ImGui, spdlog, MinHook, SafetyHook, Zydis, DirectXTK and others; see `fc5vr\extern\vrframework\cmake`).
3. **The NVIDIA DLSS SDK** (not included: NVIDIA's licence). Download it from https://github.com/NVIDIA/DLSS
   (this release used DLSS 3.7.20) and copy into this folder:
   - `deps\ngx\include\` the SDK's `include` folder (`nvsdk_ngx.h` and the others)
   - `deps\ngx\lib\Windows_x86_64\nvsdk_ngx_d.lib`: find `nvsdk_ngx_d.lib` under the SDK's `lib\Windows_x86_64` folder
   - `deps\ngx\nvngx_dlss.dll` from the SDK's `lib\Windows_x86_64\rel` folder (shipped next to the mod; the build
     does not need it)

## Build

Run `build.bat` from this folder (a normal command prompt is fine; it sets up MSVC itself).
The first build configures CMake and downloads the dependencies, which takes a while; later builds are incremental.

Outputs:
- `fc5vr\build\build-release-msvc\dxgi.dll` (and `dxgi.pdb`)
- `launcher\out\FC5VR_Launcher.exe`

## Install your build

Copy `dxgi.dll` and `FC5VR_Launcher.exe` into the game's `bin` folder, together with `openxr_loader.dll`
(`fc5vr\shipping\openxr_loader.dll`) and `nvngx_dlss.dll`. The release zip's README explains where `bin` is for Steam,
Ubisoft Connect and Epic copies, and the game options the mod needs.

## Where things are

- `fc5vr\src\FarCry5\` the mod (stereo and AFW rendering, motion controls, scopes, HUD, DLSS per eye, F11 menu).
  `FarCry5Defaults.h` holds the default settings, written as `fc5vr_settings.txt` when that file is missing.
- `fc5vr\extern\vrframework\` VR Framework (OpenXR, controllers, ImGui), based on praydog's REFramework.
- `launcher\FC5VR_Launcher.cs` the launcher (built with the .NET Framework C# compiler).

Licences: `fc5vr\LICENSE` and `fc5vr\extern\vrframework\LICENSE` (MIT); each downloaded dependency keeps its own.
Thanks to everyone listed in `ACKNOWLEDGMENTS.txt`.
