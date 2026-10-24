FAR CRY 5 VR MOD - {RELEASE}, build of {DATE} (commit {SHA})

This is an early alpha: expect rough edges (see KNOWN_ISSUES.txt).
It works with the Steam copy and the Ubisoft Connect copy of Far Cry 5. A copy bought on the Epic Games Store
runs through Ubisoft Connect too: follow the Ubisoft Connect steps for it.

1. INSTALL BEFORE
- ViGEmBus driver (the controllers reach the game as a virtual Xbox pad):
  https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe
- An OpenXR runtime set as the system default. Tested: Quest 3 with Virtual Desktop and VDXR
  (Virtual Desktop Streamer > OpenXR Runtime: VDXR). Quest Link, SteamVR and the others should work but are untested.
- An NVIDIA RTX graphics card (the mod uses DLSS).

2. FIND THE GAME FOLDER (the folder that holds the "bin" and "data_final" folders; FarCry5.exe is in bin)
- Steam: in the Steam library right-click Far Cry 5 > Manage > Browse local files.
  Usually: C:\Program Files (x86)\Steam\steamapps\common\FarCry5
  (or <your Steam library folder>\steamapps\common\FarCry5 on another drive).
- Ubisoft Connect: Games > Far Cry 5 > Properties (or Manage) > Local files: "Open folder".
  Usually: C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Far Cry 5
- Epic Games Store: Library > Far Cry 5 > the three dots > Manage > Installation (folder icon).
  Usually: C:\Program Files\Epic Games\FarCry5

3. INSTALL THE MOD
- The zip holds one folder, "bin", laid out like the game's. Extract the zip into the game folder (or drag the zip's
  bin folder onto the game folder) and let Windows merge the folders. Every file lands in the game's bin folder, next
  to FarCry5.exe. The mod itself is dxgi.dll, openxr_loader.dll, nvngx_dlss.dll, FC5VR_Launcher.exe and
  fc5vr_settings.txt; the other .txt files are to read. A mod manager that installs into the game folder works too.
- Check: dxgi.dll and FC5VR_Launcher.exe must sit next to FarCry5.exe. If you see bin\bin\dxgi.dll, the zip went
  one folder too deep: move the files of the inner bin up into the game's bin.
  Under C:\Program Files (the usual Ubisoft Connect place) Windows asks for administrator permission: allow it.
- If Windows asks, replace the old files. fc5vr_settings.txt holds the recommended mod settings: replace it when you
  update from an older version of the mod. If it is missing, the mod writes the same settings itself at the first start.
- Uninstall: in the game's bin folder delete dxgi.dll, openxr_loader.dll, nvngx_dlss.dll, FC5VR_Launcher.exe,
  the .txt files of this zip, every file and folder starting with fc5vr_, vr_config.txt and ref_ui.ini.

4. START THE GAME WITH THE LAUNCHER
- Run FC5VR_Launcher.exe from the bin folder (a desktop shortcut to it is handy), pick your settings, press Play.
  Each choice has a short explanation under it in the launcher.
  Mode: VR (in the headset) or Flatscreen (the normal game on the monitor, the mod turned off).
  Rendering: Stereo (both eyes every frame, sharpest) or AFW (one eye per frame, faster; early experimental,
  with visible artifacts). F8 switches in game.
  Resolution multiplier: x1.0 to x2.0. Higher is sharper and slower. Start at x1.0.
  DLSS: keep it on. Native (DLAA) is the sharpest; Quality, Balanced and Performance are faster and softer.
  Headset: pick yours. The mod reads its resolution and field of view from the headset; the launcher warns if your
  OpenXR runtime does not match the headset.
- Play starts the game the way your copy starts: the Steam copy through Steam, the Ubisoft Connect (or Epic) copy
  through its FarCry5.exe, which opens Ubisoft Connect to sign you in. Keep Steam or Ubisoft Connect running.
- Close the launcher with Close to keep your choices without starting the game. Starting the game the usual way
  (Play in Steam, Ubisoft Connect or Epic) also starts the mod, with the last choices made in the launcher.

5. REQUIRED GAME OPTIONS
The launcher checks these in the game's options file (Documents\My Games\Far Cry 5\gamerprofile.xml) when you
press Play in VR. If some are not set, it lists them and offers to set them for you (the first time it keeps a copy
of the file as gamerprofile.xml.fc5vr_backup). You can also set them yourself in the game's Options.
- Window mode: Borderless (not Fullscreen). In VR the mod makes the game's picture headset-sized in a window of
  its own, so exclusive Fullscreen is not needed and is not tested.
- Field of view: 120
- Anti-aliasing: SMAA
- V-Sync: Off
- Frame rate limit: Off
- Motion blur: Off
- Resolution scale: 100 %, dynamic resolution: Off
- HDR: Off
- Controls: the game's default gamepad layout (not checked by the launcher)
- Crosshair: Off (Options > Interface > Reticle: Off). The game's crosshair stays fixed in the middle of the HUD and
  does not follow the gun in your hand.

Tips: F11 opens the mod's menu. If the picture looks blurry on the very first start, restart the game once.
When something looks wrong, send what you did, Stereo or AFW, a short video if you can, and fc5vr_log.txt from the
bin folder (right after closing the game).

IF THE GAME DOES NOT START IN VR (it closes, stays black, or starts flat)
- Start it with FC5VR_Launcher.exe. After Play the launcher waits, minimised, until the game closes. If the game
  closed during the start, crashed, or VR never started, it says so and offers to save a report:
  FC5VR_report_<date>.zip in the bin folder (the folder opens with it selected). Send that zip.
  The launcher's "Make a report" button makes the same zip at any time (for example after a start that hung).
- The zip holds the mod's log (fc5vr_log.txt: how far the start got, step by step), the mod's crash report and
  dump when it wrote one (fc5vr_logs\crash_<date>.txt and .dmp), the mod's settings files, your VR runtime's log,
  your system details (Windows, graphics card and driver, OpenXR runtime and layers, Visual C++ runtime, ViGEmBus,
  the DLLs in bin) and Windows' error events for Far Cry 5. Nothing is uploaded: you send it yourself.
- Before Play, read the warnings under Headset in the launcher: a missing OpenXR runtime, other mods' DLLs in bin
  (openvr_api.dll, d3d11.dll, dinput8.dll, version.dll, winmm.dll, ReShade), an old or missing Visual C++ runtime or a
  missing ViGEmBus driver can each stop VR from starting.

Thanks to the Flat2VR community and to every project this mod builds on: see ACKNOWLEDGMENTS.txt.
