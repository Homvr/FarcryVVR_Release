# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

VR Framework is a C++ static library for enabling VR functionality in games. It's built as an extension to praydog's REFramework and supports:
- D3D11 and D3D12 rendering paths
- OpenXR and OpenVR runtimes
- Alternate Eye Rendering (AER) optimization

## Build Commands

```bash
# Configure (requires CMake 3.27+)
cmake . -B build

# Build
cmake --build build --config Release

# Format code
clang-format -i <file>
```

### CMake Options
- `COMPILE_SHADERS` - Compile HLSL shaders to CSO/header files (requires dxc.exe)
- `INVERT_DEPTH_IN_SHADER` - Invert depth in motion vector correction shader
- `SHADER_GOW_FIX` - God of War shader fix (passed to shader compiler)
- `SIGNATURE_SCAN` - Enable signature-based function scanning
- `POLYHOOK2_FUNCTION_HOOK` - Enable PolyHook2 integration instead of minhook

### Compile Definitions
Define these in your consuming project or via CMake:

**Core Behavior:**
- `HOOK_DX12_FIRST` - Prioritize D3D12 detection over D3D11 (default: D3D11 first)
- `HOOK_XINPUT` - Enable XInput hooking for controller input
- `VRMOD_EXPERIMENTAL` - Enable experimental D3D12 hooks (SetRenderTargets, SetScissorRects, SetViewports)

**Upscaler Features:**
- `MOTION_VECTOR_REPROJECTION` - Enable motion vector reprojection fix for NVIDIA/AMD upscalers
- `STREAMLINE_DLSS_RR` - Enable DLSS Ray Reconstruction support

**Debug/Profiling:**
- `DEBUG_PROFILING_ENABLED` - Enable scope profiler (requires `_DEBUG` build)
- `VERBOSE_D3D11` - Enable verbose logging for D3D11 component

**Frame Handling:**
- `OPENXR_FRAME_CROP_COPY` - Use cropped copy for OpenXR frame submission


## Architecture

### Core Components

**Framework (`src/Framework.hpp`)** - Global singleton (`g_framework`) managing:
- Module lifecycle and D3D hook monitoring
- Frame timing and present synchronization
- Mouse/keyboard input state

**Mod System (`src/Mod.hpp`)** - Plugin architecture:
- `Mod` base class with lifecycle hooks: `on_initialize()`, `on_frame()`, `on_present()`, `on_post_present()`, `on_draw_ui()`
- `ModValue<T>` templates for config values with automatic ImGui UI: `ModToggle`, `ModFloat`, `ModSlider`, `ModInt32`, `ModCombo`, `ModKey`
- Config serialization via `on_config_load()`/`on_config_save()`

**VR Module (`src/mods/VR.hpp`)** - Main VR implementation extending `Mod`:
- Manages D3D11Component and D3D12Component for rendering
- Controller input mapping (trigger, grip, joystick, buttons)
- IPD management and AER support

**D3D Hooks (`src/D3D11Hook.hpp`, `src/D3D12Hook.hpp`)** - Rendering pipeline hooks:
- Present, SetRenderTargets, SetViewports, SetScissorRects
- Swapchain management and descriptor tracking

**VR Runtimes (`src/mods/vr/runtimes/`)** - Abstraction layer:
- `VRRuntime` abstract base class
- `OpenVR.hpp` for SteamVR
- `OpenXR.hpp` for Khronos OpenXR

### Key Patterns

**Dual Rendering Path**: Code frequently branches on D3D version:
```cpp
if (is_dx12) { /* D3D12 path */ } else { /* D3D11 path */ }
```

**Hook Callbacks**: Frame synchronization via lambda callbacks to Framework:
- `on_frame()` - BeginRendering (ImGui allowed)
- `on_present()` - Present call (no ImGui)
- `on_post_present()` - After present

**Resource Management**: Extensive use of `std::unique_ptr` and `Microsoft::WRL::ComPtr`

### Framework Lifecycle

**Initialization (one-time):**
1. `Framework(HMODULE)` - Logger setup, ImGui context, starts D3D hook monitor thread
2. `hook_d3d11()`/`hook_d3d12()` - Installs D3D hooks (Present, ResizeBuffers, etc.)
3. `initialize()` - Called on first Present, sets up Windows message hook
4. `initialize_game_data()` - Background thread initializes mods via `Mods::on_initialize()`
5. `first_frame_initialize()` - Once game data ready, calls `Mods::on_initialize_d3d_thread()` on D3D thread

**Per-Frame Mod Callbacks (in order):**
1. `on_pre_imgui_frame()` - Before ImGui frame, can override input
2. `on_frame()` - During ImGui frame, can use ImGui for rendering
3. `on_present()` - During Present call, NO ImGui allowed
4. `on_post_frame()` - After ImGui rendering complete
5. `on_post_present()` - After Present returns

**D3D12-Only Callbacks:**
- `on_d3d12_set_render_targets()` - Hooked RSSetRenderTargets
- `on_d3d12_set_viewports()` - Hooked RSSetViewports
- `on_d3d12_set_scissor_rects()` - Hooked RSSetScissorRects
- `on_d3d12_create_render_target_view()` - Hooked CreateRenderTargetView

**Other Callbacks:**
- `on_device_reset()` - D3D device reset/resize
- `on_draw_ui()` - ImGui UI rendering
- `on_message()` - Windows message handling
- `on_xinput_get_state()` - XInput state interception

## Code Style

Based on WebKit style (see `.clang-format`):
- Column limit: 180 characters
- Tab width: 2 spaces (no tabs)
- Line ending: CRLF
- Braces on own line for classes, structs, namespaces, functions

### Code as Documentation (STRICT RULE)

**DO NOT write comments.** Code must be self-documenting through clear naming and structure.

The only exception: comments are allowed when explaining implicit or non-obvious logic that cannot be made clear through code alone (e.g., hardware quirks, API workarounds, undocumented behavior).

## Key Dependencies

- **spdlog** - Logging
- **glm** - Math (left-handed, DirectX conventions)
- **minhook/safetyhook** - Function hooking
- **DirectXTK/DirectXTK12** - DirectX toolkit
- **OpenXR SDK** - VR runtime
- **OpenVR** - SteamVR runtime (vendored in `extern/openvr/`)
- **ImGui** - UI
- **FidelityFX** - AMD upscaling
- **NVIDIA Streamline** - NVIDIA upscaling