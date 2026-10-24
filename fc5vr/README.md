# starfield2vr

starfield2vr is a VR modding project for Starfield that aims to deliver immersive gameplay. This project is based on [PrayDog's REFramework](https://github.com/praydog/REFramework/).

## Features

- **Full 6DoF Head Tracking** - Six degrees of freedom for immersive VR experience
- **Roomscale Support** - Standing origin with configurable world scale
- **Configurable HUD** - Adjustable HUD scale and distance for comfortable viewing
- **OpenXR Resolution Scale** - Customize rendering resolution for your headset
- **Decoupled Pitch** - Independent head movement from character orientation
- **Desktop Recording Fix** - Option to fix desktop mirror for recording/streaming
- **Haptic Feedback** - Controller vibration support for immersive gameplay
- **Quad View Support** - Compatible with Pimax and Virtual Desktop
- **Multiple Controller Support** - Works with Oculus Touch, Vive wands, and Valve Index controllers

## Prerequisites

- Set your VR headset as the default OpenXR engine
- Install ViGEmBus driver for motion controllers to work as joystick:
  [ViGEmBus v1.22.0](https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe)

## Installation

### Steam
Copy content from archive `starfield2vr-{openxr/openvr}-vX.X.X.zip` into:
```
C:\Program Files (x86)\Steam\steamapps\common\Starfield
```

### Gamepass
Copy content from archive `starfield2vr-{openxr/openvr}-vX.X.X.zip` into:
```
C:\XboxGames\Starfield\Content
```

## In-game Settings

- **Display:** Set to Windowed
- **Frame Generation:** Off
- **VSYNC:** Off
- **Motion Blur:** Off
- **Depth of Field:** Off
- **Dynamic Resolution:** Off (recommended)

## Notes

- In-game Overlay has F11 hotkey (works only on main display, not in HMD)
- Mod is based on [PrayDog's REFramework](https://github.com/praydog/REFramework/)

## Overlay Settings (F11 Menu)

The in-game overlay provides configurable options:
- **OpenXR Resolution Scale** - Adjust VR rendering resolution (0.1x to 5.0x)
- **Desktop Recording Fix** - Enable for better desktop mirror quality
- **Set Standing Origin Key** - Recenter your VR position

## Controller Layout

| Control | Action |
|---------|--------|
| Thumbrest | Left/Right buttons respectively |
| Left Grip + Left Stick Button | Turn screen to flat screen |
| Left Grip + Left Joystick | Acts as cross in joystick |
| Right Grip | Aim (joystick left trigger) |
| Left Grip + System Button | Xbox Start or system menu button (Switches between third-person/first-person views) |

## Additional Tips

- TAA, DLSS, and CAS will work; Frame Generation will not
- **Remember to never turn on Frame Generation**

## Known Limitations/Issues

- OpenVR missing bindings for controllers

## Video Tutorials

- [Starfield VR Setup Guide by Good Samaritan](https://youtu.be/UKt2utvotxA?si=2EJMFvhelMTlFuHG)
- [Starfield VR Tutorial by ParadiseDecay](https://youtu.be/cJ9ccj92xNM?t=475&si=15dW7ORzva_0O0GM)
