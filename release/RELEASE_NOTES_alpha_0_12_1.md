Far Cry 5 VR, alpha 0.12.1 (25 October 2026): a hotfix for alpha 0.12.
True stereo rendering in your PC VR headset, 6DoF head tracking and motion controls with the gun in your hand.

**This is an early alpha.** Expect rough edges; see KNOWN_ISSUES.txt in the zip.

## Downloads
- `FC5VR_alpha_0_12_1.zip`: the mod, built from this repository by GitHub Actions. It holds one folder, `bin`, laid out
  like the game's: extract it into the Far Cry 5 game folder (the one with `bin` and `data_final`). It contains no
  other archive.
- The source is this repository (BUILDING.md explains the build).

## Fixed since alpha 0.12
- **Stereo:** no more left-eye tremor for players whose VR Framework setting had VR_AsyncAER on (it is now always
  off).
- **AFW:** two image rebuild fixes for cleaner edges in AFW are now on by default.
- **Crash fix:** the game no longer crashes when the PC has an empty D: drive (a card reader or a disc drive with
  nothing in it).

Earlier changes are listed in CHANGELOG.md.

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
