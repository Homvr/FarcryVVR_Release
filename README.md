# Far Cry 5 VR

An unofficial mod that turns Far Cry 5 into a full VR game: true stereo 3D through OpenXR, a weapon that sits in
your hand and shoots where the barrel points, body gestures to switch and sling weapons, a HUD that floats in front
of you, and comfort options for driving, flying and cutscenes.

> **Early alpha.** Expect rough edges. Read [KNOWN_ISSUES.txt](release/KNOWN_ISSUES.txt) before you report something.

**[Download the latest release](https://github.com/Homvr/FarcryVVR_Release/releases)** (the `FC5VR_<version>.zip` file)

---

## Features

### Rendering
- **Native stereo through OpenXR.** Both eyes are rendered every frame from their own viewpoint, with the headset's
  own resolution and field of view. Full 6DoF head tracking, roomscale.
- **AFW mode** (early, experimental): one eye per frame, the other rebuilt from depth. Faster, with visible artifacts.
  Switch between Stereo and AFW with **F8**.
- **NVIDIA DLSS per eye**: Native (DLAA), Quality, Balanced or Performance, with a choice of DLSS model and sharpening.
- **Resolution multiplier** (x1.0 to x2.0) and a **field of view** slider that trades the lens edges for a sharper
  centre.
- **Far detail at the flat-screen level** (optional): keeps the game's level of detail as it is on a monitor.
- **Clean picture in the headset**: no depth-of-field blur on your sights, no vignette or film grain that differs
  between the eyes, no chromatic aberration.
- **World scale** setting.
- Works with **OFXR Bridge** running, including its DLSS frame generation (recommended, see below).

### Motion controls
- **The weapon is in your hand.** Shots leave along the barrel, with fast shot release so the gun fires the moment you
  pull the trigger, even while it moves.
- **Two-handed hold**: put the left hand on the fore-end and both hands line up on the gun.
- **Hand models** that follow the controllers, and an optional floating-weapon mode.
- **Sights that work**: shots go through the centre of the red dot, the scope or the iron sights, read from each gun's
  own aim-down-sights pose.
- **Red dot and aim dot** options, from realistic (dot only in reflex sights) to an aim dot on every gun.
- **Magnified scopes**: bring a scope to your eye to see a magnified picture in the eyepiece. With two hands on the
  gun, the right stick zooms in steps (x1, x4, x8, x16, x32). An optional scope render pass keeps far detail sharp.
- **Steady weapon**: no running bob, sprint pose or recoil animation moving the gun in your hand (reloads still play).
  Optional weapon smoothing for shaky hands, no recoil kick and no weapon sway.
- **Haptics**: the game's rumble goes to the hand holding the gun, plus a firm pulse on every shot (strength and length
  adjustable).
- **Bullet tracers from the muzzle**, flamethrower flames that stay in the world, and an optional higher bullet speed.
- **Walking follows your head**, not the gun. Smooth or snap turning. Left-handed mode (not tested).

### Gestures
- **Sling**: your main weapon hangs across your chest. Grab it with the right grip to hold it, let go to put it back.
- **Shoulder weapon slots**: reach to your right or left shoulder and press the grip to draw a weapon slot (left
  shoulder slot 1, right shoulder slot 2 by default; each can take any of the slots 1 to 4).
- **Holster** (experimental, off by default): your sidearm on the right hip, grabbed with the right grip.
- **Throwables and pouch** (experimental, off by default): grenades and molotovs in a pouch on your chest. Grab one,
  aim with a trajectory arc, let go of the grip to throw.
- **Melee by swinging** (experimental, work in progress).
- Every gesture zone can be moved, resized, shown in the headset to check where it is, and reset to its default.
  A light buzz tells you when your hand is in a zone.

### HUD and menus
- **HUD on its own panel** in front of you, with adjustable distance, width and height (or in the image, or hidden).
- **World markers on their targets**: objective and waypoint markers leave the HUD panel and sit on what they point at,
  in both eyes, with size limits and smoothing.
- **Menus, the map and loading screens** on a flat screen in the room. The map stays usable with the controllers.
- **Binoculars** magnify the view, as on a monitor.
- **B closes tutorial pop-ups.**
- A **"VR mod loading" popup** after each load, until the mod has finished setting up your weapons (the launcher can
  skip it).

### Vehicles, flying and comfort
- **The view turns with the vehicle**: the cabin stays put around you and the road turns. When flying (helicopters,
  planes, parachute, wingsuit) the view also climbs, dives and banks with the aircraft while you look around freely.
  You can also keep the horizon always level, or turn the view only with the stick.
- **Helicopter and plane controls**: the right stick steers the aircraft and pitches a helicopter's nose; in cars,
  quads and boats the right stick never moves the view.
- **Drive with the left hand, shoot with the right**: left trigger throttle, left grip brake, right trigger fires.
- An automatic **recenter** after vehicles and loading screens, and optional comfort settings: a **vignette** in
  vehicles and a **fade to black** while getting in or out.
- **Cutscenes** in 3D with the horizon held level, or on a flat screen in the room.
- **Smooth walking and running**, a **body-based view** that removes the game's head bob, and parachute and fall
  handling that hands the view back as soon as you land.

### Launcher
- **FC5VR_Launcher.exe** picks VR or flat screen, Stereo or AFW, the resolution multiplier, DLSS and your headset.
- It checks the game options the mod needs and offers to set them for you.
- It warns about a missing OpenXR runtime, a missing ViGEmBus driver, an old Visual C++ runtime or other mods' DLLs
  that would stop VR from starting.
- If the game fails to start in VR or crashes, it offers a **report zip** (log, crash dump, system details) for you to
  send. Nothing is uploaded.

---

## Requirements

- **Far Cry 5** on PC: the Steam copy or the Ubisoft Connect copy. An Epic Games Store copy runs through Ubisoft
  Connect and works the same way.
- **Windows 10 or 11**, 64-bit.
- **An NVIDIA RTX graphics card** (the mod uses DLSS).
- **A PC VR headset with an OpenXR runtime set as the system default.** Tested: Meta Quest 3 with Virtual Desktop and
  VDXR (Virtual Desktop Streamer > OpenXR Runtime: VDXR). Quest Link / Air Link, SteamVR, Pimax Play and others
  should work but are untested. The launcher has presets for Quest 2, 3, 3S and Pro, Valve Index, Vive Pro 2,
  Reverb G2, Bigscreen Beyond and Pimax Crystal.
- **The ViGEmBus driver** (the controllers reach the game as a virtual Xbox pad):
  [ViGEmBus 1.22.0](https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe)
- **The Microsoft Visual C++ 2015-2022 x64 runtime** (`vc_redist.x64.exe` from Microsoft), if it is not already
  installed. The launcher tells you.

For reference, on an RTX 4090: about 50 fps in Stereo and 80+ in AFW.

### Strongly recommended: OFXR Bridge and a high-bitrate codec

Far Cry 5 is full of trees, grass and bushes, and foliage is what suffers most in VR: a low frame rate makes it judder,
and a compressed stream turns it into a blurry, blocky smear.

- **Use [OFXR Bridge](https://github.com/tig3rmast3r/OFXR-Bridge/releases)** (frame generation for OpenXR games).
  The mod works with it, including its DLSS frame generation, and it makes the game far smoother in the headset.
- **Stream with a high-bitrate codec** if your headset is wireless or streams over a cable (Quest with Virtual Desktop
  or Quest Link). In Virtual Desktop: **H.264+ at 300-400 Mbps** if your Wi-Fi holds it, or **AV1 10-bit at about
  200 Mbps**, with **automatic bitrate turned off**. Lower bitrates and the default codecs blur the foliage badly.

---

## Install

1. **Install the requirements above** (ViGEmBus, your OpenXR runtime as the default). We strongly recommend
   [OFXR Bridge](https://github.com/tig3rmast3r/OFXR-Bridge/releases) and a high-bitrate codec (H.264+ at 300-400 Mbps
   or AV1 10-bit at about 200 Mbps, automatic bitrate off): see
   [Strongly recommended](#strongly-recommended-ofxr-bridge-and-a-high-bitrate-codec).

2. **Find the game folder**: the folder that holds the `bin` and `data_final` folders (`FarCry5.exe` is in `bin`).
   - **Steam**: right-click Far Cry 5 in the library > Manage > Browse local files.
     Usually `C:\Program Files (x86)\Steam\steamapps\common\FarCry5`
   - **Ubisoft Connect**: Games > Far Cry 5 > Properties > Local files: Open folder.
     Usually `C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Far Cry 5`
   - **Epic Games Store**: Library > Far Cry 5 > ... > Manage > Installation.
     Usually `C:\Program Files\Epic Games\FarCry5`

3. **Extract the zip into the game folder.** The zip holds one folder, `bin`, laid out like the game's: let Windows
   merge it with the game's `bin` folder and replace the old files if asked. Every file lands next to `FarCry5.exe`:

   | File | What it is |
   | --- | --- |
   | `dxgi.dll` | The mod |
   | `openxr_loader.dll` | OpenXR loader |
   | `nvngx_dlss.dll` | NVIDIA DLSS runtime |
   | `FC5VR_Launcher.exe` | The launcher |
   | `fc5vr_settings.txt` | The recommended mod settings |
   | `FC5VR_README.txt`, `KNOWN_ISSUES.txt`, `ACKNOWLEDGMENTS.txt`, `LICENSE.txt` | To read |

   **Check:** `dxgi.dll` and `FC5VR_Launcher.exe` must sit next to `FarCry5.exe`. If you end up with
   `bin\bin\dxgi.dll`, move the files of the inner `bin` up one level. Under `C:\Program Files` Windows asks for
   administrator permission: allow it.

   **Updating** from an older version: replace every file, including `fc5vr_settings.txt` (it holds the
   recommended settings for this version). If it is missing, the mod writes the same settings at its first start.

4. **Start the game with `FC5VR_Launcher.exe`** (a desktop shortcut to it is handy). Pick:
   - **Mode**: VR, or Flatscreen (the normal game, mod off).
   - **Rendering**: Stereo (sharpest, recommended) or AFW (faster, experimental).
   - **Resolution multiplier**: start at x1.0.
   - **DLSS**: keep it on. Native (DLAA) is the sharpest; Quality, Balanced and Performance are faster.
   - **Headset**: yours.

   Press **Play**. Keep Steam or Ubisoft Connect running. After the first time, starting the game the usual way also
   starts the mod with your last launcher choices.

### Game options the mod needs

The launcher checks these when you press Play in VR and offers to set them (it keeps a backup of your options file,
`Documents\My Games\Far Cry 5\gamerprofile.xml.fc5vr_backup`). You can also set them in the game's Options:

| Option | Value |
| --- | --- |
| Window mode | Borderless |
| Field of view | 120 |
| Anti-aliasing | SMAA |
| V-Sync | Off |
| Frame rate limit | Off |
| Motion blur | Off |
| Resolution scale | 100 %, dynamic resolution off |
| HDR | Off |
| Reticle (Interface) | Off |
| Controls | The game's default gamepad layout |

---

## The F11 menu

Press **F11** on the keyboard, or **click both sticks** in the headset, to open the mod's menu. It shows in the
headset and on the monitor. In the headset, point with the controller and press **A** or the **trigger**; the right
stick scrolls, and left / right nudges the slider under the pointer.

At the top, choose how many settings are shown: **Basic**, **Advanced** or **Debug**. Every change is saved at once
in `fc5vr_settings.txt`. The tabs:

| Tab | What is in it |
| --- | --- |
| **Display** | Stereo or AFW, field of view, far detail, DLSS (on / off, render scale, model, sharpening), resolution multiplier, picture clean-ups, world scale |
| **HUD and menus** | Where the HUD goes and its size, world markers, tutorial pop-ups, menus / map / loading screens, binoculars, where this menu hangs in the headset |
| **Gestures** | Each gesture on or off, its zone size and place: shoulder weapon slots, sling, holster, throwables and pouch, melee |
| **Motion control** | Motion controls on / off, left-handed, hand models, hip-fire spread, shot release, recoil and sway, walking direction |
| **Weapon** | Smoothing, vibration, bullet tracers and flames, bullet speed, sights, red dot, scopes, the weapon's angle on the controller |
| **Comfort** | Frame pacing, smooth walking, body-based view, snap or smooth turning, the gun's animation, cutscenes, vehicles, flying, recentering, parachutes |
| **Layout** | The full controller layout, on foot and in vehicles |
| **Experimental** | Unfinished features, off by default (Advanced level) |

### Keys

| Key | Action |
| --- | --- |
| **F11** | Open / close the menu (or click both sticks) |
| **F9** | Recenter the view |
| **F8** | Switch Stereo / AFW |
| **F7** | Motion controls on / off |
| **F12** | Flat screen on / off (or left grip + left stick click) |

Holding the Meta button (or Virtual Desktop's recenter) also recenters the game.

---

## Controls

The mod uses the game's default gamepad layout. The **Layout** tab of the F11 menu always shows the current one.

**Right controller**

| Button | On foot | Driving |
| --- | --- | --- |
| Trigger | Fire | Shoot |
| Grip | Hold the weapon (from the sling) | Hold the weapon |
| A | Jump | Change seat |
| B | Previous weapon / hold: heal; closes tutorial pop-ups | Change weapon / hold: heal |
| Stick | Turn; in the weapon wheel: pick a slot | Nothing (you look with your head); aircraft: steer |
| Stick click | Melee / takedown | Horn |

**Left controller**

| Button | On foot | Driving |
| --- | --- | --- |
| Trigger | Weapon wheel (hold, pick with the right stick) | Throttle |
| Grip | Modifier (see below), or hold the weapon with two hands | Brake / reverse |
| X | Crouch / slide | Handbrake |
| Y | Reload / hold: use | Hold: get in / out |
| Stick | Move | Steer |
| Stick click | Sprint | Auto-drive |
| Menu button | Pause | Pause |

**Left grip held (away from the weapon)**

| With | Action |
| --- | --- |
| Either stick | D-pad: up binoculars, left / right companion orders, down throw a rock (driving: right stick only, up = radio) |
| Right B | Throw (aircraft: rockets) |
| Menu button | Map |
| Left stick click | Flat screen on / off |

**Gestures** (on foot): right hand on the slung weapon + grip = weapon in hand; right hand at a shoulder + grip =
that shoulder's weapon slot.

---

## Uninstall

In the game's `bin` folder, delete:
- `dxgi.dll`, `openxr_loader.dll`, `nvngx_dlss.dll`, `FC5VR_Launcher.exe`
- `FC5VR_README.txt`, `KNOWN_ISSUES.txt`, `ACKNOWLEDGMENTS.txt`, `LICENSE.txt`
- every file and folder whose name starts with `fc5vr_` (settings, log, crash logs, learned gun poses) and any
  `FC5VR_report_*.zip`
- `vr_config.txt` and `ref_ui.ini`

The game is then back to normal. If you let the launcher change your game options, set them back in the game's
Options, or restore `Documents\My Games\Far Cry 5\gamerprofile.xml` from `gamerprofile.xml.fc5vr_backup`.

---

## If something goes wrong

- **The game does not start in VR** (it closes, stays black or starts flat): start it with `FC5VR_Launcher.exe` and
  read the warnings under Headset first. If the start fails, the launcher offers to save `FC5VR_report_<date>.zip` in
  the `bin` folder; its **Make a report** button makes one at any time.
- **Blurry picture on the very first start**: restart the game once.
- **Reporting a problem**: say what you did, Stereo or AFW, add a short video if you can, and `fc5vr_log.txt` from
  the `bin` folder (right after closing the game), or the report zip.

---

## Building from source

This repository holds the mod's source. See [BUILDING.md](BUILDING.md): Visual Studio 2022 Build Tools, Git and the
NVIDIA DLSS SDK, then `build.bat`. Releases are built by GitHub Actions ([build.yml](.github/workflows/build.yml)).
Changes per version are in [CHANGELOG.md](CHANGELOG.md).

## Credits and licence

Built on VR Framework (mutars, Fribur's fork) and starfield2vr, both based on praydog's REFramework, with ideas and
code from Dishonored VR, fc5-dlss, Luma Framework and CryHook5, and thanks to the Flat2VR community. The full list is
in [ACKNOWLEDGMENTS.txt](ACKNOWLEDGMENTS.txt).

The mod's code is under the MIT licence (`fc5vr/LICENSE`, `fc5vr/extern/vrframework/LICENSE`); each dependency keeps
its own licence. The NVIDIA DLSS runtime is distributed under NVIDIA's licence.

*Far Cry 5 is a trademark of Ubisoft. This is an unofficial fan mod, not made or endorsed by Ubisoft or NVIDIA.*
