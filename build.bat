@echo off
set NoDefaultCurrentDirectoryInExePath=
set PATHEXT=.COM;.EXE;.BAT;.CMD;.VBS;.JS;.WS;.MSC
set PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer
rem MSVC 2022: the Build Tools, else the newest Visual Studio with C++ (vswhere).
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (echo MSVC 2022 with C++ not found: install Visual Studio 2022 Build Tools, "Desktop development with C++" & exit /b 1)
call "%VCVARS%" >nul
rem NVIDIA DLSS SDK headers and library (not in git): deps\ngx\include and deps\ngx\lib\Windows_x86_64\nvsdk_ngx_d.lib.
if not exist "%~dp0deps\ngx\include\nvsdk_ngx.h" (echo deps\ngx missing: copy the DLSS SDK's include and lib folders there, see BUILDING.md & exit /b 1)
rem Dependency sources: local copies on the builder's PC when they exist (same pinned commits), otherwise CMake
rem downloads them from GitHub at the first configure. FC5VR_ONLINE_DEPS=1 forces the download.
set SF=D:/Claude_dev/Starfield/repo/build/build-release-msvc/_deps
set DX=D:/SteamLibrary/steamapps/common/FarCry5/VRMod_dev/deps
set LOCALDEPS=
if not "%FC5VR_ONLINE_DEPS%"=="1" if exist "%SF%/zydis-src" if exist "%DX%/directxtk-src" set LOCALDEPS=-DFETCHCONTENT_SOURCE_DIR_BDDISASM=%SF%/bddisasm-src -DFETCHCONTENT_SOURCE_DIR_DIRECTXTK=%DX%/directxtk-src -DFETCHCONTENT_SOURCE_DIR_DIRECTXTK12=%DX%/directxtk12-src -DFETCHCONTENT_SOURCE_DIR_FIDELITYFX=%SF%/fidelityfx-src -DFETCHCONTENT_SOURCE_DIR_GLM=%SF%/glm-src -DFETCHCONTENT_SOURCE_DIR_IMGUI=%SF%/imgui-src -DFETCHCONTENT_SOURCE_DIR_IMGUIZMO=%SF%/imguizmo-src -DFETCHCONTENT_SOURCE_DIR_KANANLIB=%SF%/kananlib-src -DFETCHCONTENT_SOURCE_DIR_MINHOOK=%SF%/minhook-src -DFETCHCONTENT_SOURCE_DIR_OPENXR=%SF%/openxr-src -DFETCHCONTENT_SOURCE_DIR_SAFETYHOOK=%SF%/safetyhook-src -DFETCHCONTENT_SOURCE_DIR_SPDLOG=%SF%/spdlog-src -DFETCHCONTENT_SOURCE_DIR_STREAMLINE=%SF%/streamline-src -DFETCHCONTENT_SOURCE_DIR_VIGEMCLIENT=%SF%/vigemclient-src -DFETCHCONTENT_SOURCE_DIR_XBYAK=%SF%/xbyak-src -DFETCHCONTENT_SOURCE_DIR_ZYDIS=%SF%/zydis-src
cd /d "%~dp0fc5vr"
if not exist build\build-release-msvc\build.ninja cmake --preset build-release-msvc -DCMAKE_POLICY_VERSION_MINIMUM=3.5 %LOCALDEPS% || exit /b 1
cmake build\build-release-msvc >nul || exit /b 1
if exist build\build-release-msvc\dxgi.lib del build\build-release-msvc\dxgi.lib
cmake --build build\build-release-msvc || exit /b 1

if exist "%~dp0launcher\build_launcher.bat" call "%~dp0launcher\build_launcher.bat" >nul || exit /b 1
