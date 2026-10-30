# Changelog

## alpha 0.15 (30 October 2026): the gesture update
Full notes: [release/RELEASE_NOTES_alpha_0_15.md](release/RELEASE_NOTES_alpha_0_15.md).
- **Gestures:** weapon slots on both shoulders (slots 1 and 2 by default), the sling refilled after a load, a
  holster on the hip and a cloth pouch for throwables (both experimental), faster weapon switching.
- **Vehicles and falls:** the right stick steers helicopters and planes and never moves the view in a vehicle,
  helicopter nose pitch, left hand drives and right hand shoots, planes face the nose on entry, boats accelerate,
  parachute and wingsuit views fixed, no bounce after a knockdown, 3D cutscenes follow cuts.
- **View and HUD:** body-based view, smooth walking and running, small detection chevrons placed in the world,
  big world markers limited, a "VR mod loading" popup after each load (the launcher can skip it), map and
  binoculars, B closes tutorial pop-ups, a tidier F11 menu.
- **Picture:** OFXR Bridge's DLSS frame generation works, AFW foliage steady, lamps placed per eye, a foliage test
  option for DLSS.
- **Fixes:** no more lasting FPS drop from the 5-minute weapon scan, shots no longer held back, no automatic aim down
  sights on iron sights, the weapon angle on the controller is saved.
- **Defaults:** weapon pitch on the controller -5 degrees, even frame pacing off.
- **Recommended:** OFXR Bridge and a high-bitrate codec (H.264+ 300-400 Mbps or AV1 10-bit about 200 Mbps).

## alpha 0.12.1 (25 October 2026): hotfix
- **Stereo:** no more left-eye tremor for players whose VR Framework setting had VR_AsyncAER on (it is now always
  off).
- **AFW:** two image rebuild fixes for cleaner edges in AFW are now on by default.
- **Crash fix:** the game no longer crashes when the PC has an empty D: drive (a card reader or a disc drive with
  nothing in it).

## alpha 0.12 (24 October 2026)
- **OFXR:** the mod now works with OFXR Bridge running (3D and VR instead of a flat image), confirmed in the headset.
  It also avoids a stall while loading.
- **AFW:** the gun no longer doubles up between the eyes when you move, the car interior stays steady while
  driving, and the view no longer judders going up or down hills.
- **Scopes:** guns without a scope (such as the M60 with no sights) are no longer treated as scoped.
- **Vibration:** every shot gives a firm, even pulse in the gun hand. Its strength and length can be set in
  F11 > Weapon > Feel.
- **Flamethrower:** flames, muzzle smoke and sparks stay where they were fired instead of swinging with your hand.
  Fired flames stay in the world for 1.5 s after you let go (F11 > Weapon > Bullet tracers).
- **Flamethrower pilot flame:** stays on the nozzle when not firing (new, not yet checked in the headset).

## alpha 0.11 (24 October 2026)
- **Scopes:** with two hands on the gun, the right stick zooms in steps (x1, x4, x8, x16, x32). Shadows outside the
  scope stay sharp, zooming out no longer turns the scope black, and sniper zoom levels are read correctly.
- **Red dots:** closed red dot sights no longer show a dark glass disc. Scope and red dot settings moved to F11 > Weapon.
- **Guns:** both eyes show the gun in the same place, two-handed guns line up with both hands, shots leave at once
  while the gun moves, and each gun's grip is remembered.
- **Hands:** empty hands stay on the controllers while walking, running or sprinting. The left grip no longer grabs
  the Uzi.
- **Gestures:** gesture zones can be shown and moved, each with a Default button. The shoulder zone (switch weapon)
  now sits behind the ear and turns with the body.
- **Vibration:** goes to the hand holding the gun (both hands with a two-handed hold), with a strength slider.
- **Movement:** walking follows the headset and stays straight while the gun turns. No walking bob. Getting in and
  out of vehicles is smoother, and the view recenters after loading.
- **Picture:** no black squares in far shadows, fewer stutters, and button prompts stay on the HUD panel. AFW is
  marked experimental (Stereo recommended).
- **Launcher:** checks the game options the mod needs and offers to set them. A crash logger offers a report zip.
- **F11 menu:** cleaned up.
- **Package:** the zip holds one `bin` folder laid out like the game's and contains no other archive.

## pre_alpha_0_1 (20 October 2026)
- First public release.
