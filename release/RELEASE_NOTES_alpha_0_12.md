Far Cry 5 VR, alpha 0.12: the third tester release, after alpha 0.11 (earlier on 24 October 2026).
True stereo rendering in your PC VR headset, 6DoF head tracking and motion controls with the gun in your hand.

**This is an early alpha.** Expect rough edges; see KNOWN_ISSUES.txt in the zip.

## Downloads
- `FC5VR_alpha_0_12.zip`: the mod, built from this repository by GitHub Actions. It holds one folder, `bin`, laid out
  like the game's: extract it into the Far Cry 5 game folder (the one with `bin` and `data_final`). It contains no
  other archive.
- The source is this repository (BUILDING.md explains the build).

## New since alpha 0.11
- **OFXR:** the mod now works with OFXR Bridge running (3D and VR instead of a flat image), and loading no longer
  stalls.
- **AFW:** the gun no longer doubles up between the eyes when you move, the car interior stays steady while
  driving, and the view no longer judders going up or down hills.
- **Scopes:** guns without a scope (such as the M60 with no sights) are no longer treated as scoped.
- **Vibration:** every shot gives a firm, even pulse in the gun hand. Its strength and length can be set in
  F11 > Weapon > Feel.
- **Flamethrower:** flames, muzzle smoke and sparks stay where they were fired instead of swinging with your hand.
  Fired flames stay in the world for 1.5 s after you let go (F11 > Weapon > Bullet tracers).
- **Flamethrower pilot flame:** stays on the nozzle when not firing (new, not yet checked in the headset).

Everything new in alpha 0.11 is listed in CHANGELOG.md.

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

Updating from an earlier version: extract over it and replace the files, including `fc5vr_settings.txt`.

FC5VR_README.txt in the zip has the full guide, including the game options the mod needs.
Tested on Quest 3 with Virtual Desktop (VDXR), an RTX 4090 and the Steam copy; other headsets, runtimes and stores
should work but are untested.

## Thanks
To the Flat2VR community, and to the projects this mod builds on or learned from: VR Framework (mutars, Fribur) and
praydog's REFramework and UEVR, Dishonored VR (VR-Stereo-Hub), fc5-dlss (HenDGS) and Luma Framework (Filoppi),
CryHook5 (Force67), PureDark, Luke Ross, the Helix Mod / 3DMigoto community, ViGEm (nefarius) and the open-source
libraries listed in ACKNOWLEDGMENTS.txt.

Far Cry 5 is a trademark of Ubisoft. Unofficial fan mod, not made or endorsed by Ubisoft or NVIDIA.
