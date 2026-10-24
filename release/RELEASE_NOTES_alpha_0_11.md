Far Cry 5 VR, alpha 0.11: the second tester release, after pre_alpha_0_1 (20 October 2026).
True stereo rendering in your PC VR headset, 6DoF head tracking and motion controls with the gun in your hand.

**This is an early alpha.** Expect rough edges; see KNOWN_ISSUES.txt in the zip.

## Downloads
- `FC5VR_alpha_0_11.zip`: the mod, built from this repository by GitHub Actions. It holds one folder, `bin`, laid out
  like the game's: extract it into the Far Cry 5 game folder (the one with `bin` and `data_final`). It contains no
  other archive.
- The source is this repository (BUILDING.md explains the build).

## New since pre_alpha_0_1
**Scopes and sights**
- With the gun held in two hands, the right stick zooms the scope in steps: x1, x4, x8, x16, x32 (one step per push).
- Sun shadows outside the scope stay sharp while the scope is drawn.
- No more black scope when zooming out; sniper zoom levels are read correctly.
- Closed red dot sights no longer show their dark glass disc.
- Scope and red dot settings moved to the F11 Weapon tab.

**Hands, guns and gestures**
- Both eyes show the gun in exactly the same place (no more gun shake in one eye).
- Two hands on a gun: the gun's own hand positions line up with your hands.
- Shots leave at once while the gun is moving; each gun's grip is remembered between sessions.
- Empty hands stay on the controllers while walking, running or sprinting, each palm turned onto its controller.
- Gesture zones can be shown (F11 Gestures > show the gesture zones) and moved; each has a Default button.
  The right shoulder zone (switch weapon) now sits behind and below the ear and turns with your body, not your head.
- Controller vibration goes to the hand holding the gun (both hands with a two-handed hold), with a strength slider.
- The left grip no longer grabs the Uzi in a two-handed hold.

**Moving and comfort**
- Walking forward follows the headset, also with a weapon in hand, and stays straight while you turn the gun.
- The view stays put while the gun turns; no up-and-down bob while walking.
- Smoother getting in and out of vehicles; recenter when play starts after loading, and the headset's own recenter is
  followed.

**Picture and performance**
- No black squares in far shadows.
- Fewer periodic stutters (lighter work on the render thread; a HUD debug read-back no longer runs by default).
- Button prompts and the vehicle command list stay on the HUD panel.
- AFW is labelled early experimental in F11: Stereo is the recommended mode.

**Launcher and support**
- Before Play in VR, the launcher checks the game options the mod needs and offers to set them (it keeps a backup).
- A crash logger: if the game closes during the start or crashes, the launcher offers a report zip to send.
- The F11 menu was cleaned up (unused controls removed).

## Install (Steam, Ubisoft Connect or Epic)
1. Install the ViGEmBus driver and set your headset's OpenXR runtime as the system default. An NVIDIA RTX card is
   needed (DLSS).
2. Find the game folder:
   - Steam: right-click Far Cry 5 > Manage > Browse local files
     (usually `C:\Program Files (x86)\Steam\steamapps\common\FarCry5`)
   - Ubisoft Connect: Far Cry 5 > Properties > Local files: Open folder
     (usually `C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Far Cry 5`)
   - Epic: Far Cry 5 > Manage > Installation (it runs through Ubisoft Connect too)
3. Extract the zip into that folder and let Windows merge `bin`. `dxgi.dll` and `FC5VR_Launcher.exe` must end up next
   to `FarCry5.exe`.
4. Run `bin\FC5VR_Launcher.exe`, pick your headset and press Play.

Updating from pre_alpha_0_1: extract over it and replace the files, including `fc5vr_settings.txt`.

FC5VR_README.txt in the zip has the full guide, including the game options the mod needs.
Tested on Quest 3 with Virtual Desktop (VDXR), an RTX 4090 and the Steam copy; other headsets, runtimes and stores
should work but are untested.

## Thanks
To the Flat2VR community, and to the projects this mod builds on or learned from: VR Framework (mutars, Fribur) and
praydog's REFramework and UEVR, Dishonored VR (VR-Stereo-Hub), fc5-dlss (HenDGS) and Luma Framework (Filoppi),
CryHook5 (Force67), PureDark, Luke Ross, the Helix Mod / 3DMigoto community, ViGEm (nefarius) and the open-source
libraries listed in ACKNOWLEDGMENTS.txt.

Far Cry 5 is a trademark of Ubisoft. Unofficial fan mod, not made or endorsed by Ubisoft or NVIDIA.
