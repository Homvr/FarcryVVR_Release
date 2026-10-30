# Far Cry 5 VR alpha 0.15: the gesture update

Your weapons now live on your body. Reach over a shoulder for your main guns, grab the rifle off the sling, pull
the sidearm from its holster on your hip, and take grenades from a pouch on your chest. This update also brings a
reworked vehicle, helicopter and parachute camera, a smoother view on foot, a fix for a big FPS drop and a tidier
F11 menu.

**Strongly recommended:** use [OFXR Bridge](https://github.com/tig3rmast3r/OFXR-Bridge/releases) and a high-bitrate
codec. In Virtual Desktop: H.264+ at 300-400 Mbps if your Wi-Fi holds it, or AV1 10-bit at about 200 Mbps, with
automatic bitrate off. Foliage is what suffers most from a low frame rate and a compressed stream.

**Updating:** replace every file, including `fc5vr_settings.txt` (it holds this version's recommended settings).

## New: gestures and weapons on your body

- **Shoulder weapon slots:** reach to your left shoulder for weapon slot 1, to your right shoulder for slot 2 (the
  game's keys 1 and 2), with the right hand and the grip. Each shoulder can take any slot from 1 to 4 in
  F11 > Gestures. The new weapon shows in your hand the moment you grip.
- **Sling:** your main weapon hangs across your chest (also in both eyes in AFW). After a load, your slot 1 weapon
  is put on the sling so it is never empty.
- **Holster (experimental):** your sidearm on the right hip, in a holster shaped to the gun. Grab it with the right
  grip, let go to put it back. Turn it on in F11 > Gestures > Holster.
- **Throwables and pouch (experimental):** grenades, molotovs and dynamite in a cloth pouch on your chest, lit like
  your clothes. Grab one, aim with the trajectory arc, let go of the grip to throw. Turn it on in F11 > Gestures.
- **Melee (experimental):** melee weapons lined up with the controller, and a first version of swing-to-hit.
- **Faster weapon switching:** the game's switch animation is sped up, so the shoulders and the wheel answer sooner.
- A grip pressed just outside a gesture zone no longer turns the left stick into the D-pad.

## New: vehicles, flying and falls

- **Helicopters:** the right stick turns the helicopter and pitches its nose (forward = nose down).
- **The right stick never moves the view in a vehicle.** It steers helicopters and planes, and does nothing in cars,
  quads and boats.
- **Drive with the left hand, shoot with the right:** left trigger throttle, left grip brake, right trigger fires
  (it draws your sidearm while driving and you stay the driver).
- **Planes:** getting in, you face the nose; flying, you look around freely.
- **Boats** accelerate again.
- **Vehicle detection now comes from the game itself**, so getting in and out, seats and quads are read correctly.
- **Parachute and wingsuit:** the view no longer jumps when the wingsuit or the chute opens, and it is handed back
  within a second of landing, even running downhill. A jump off a balcony is no longer taken for a parachute.
- **Knocked down:** the view stays with you, with no 1.6 m bounce when you get up.
- **3D cutscenes** turn with the scene's camera on cuts and big turns, with the horizon kept level.

## New: view, HUD and menus

- **Body-based view:** your eyes stand on the player's body, so turning or raising the gun no longer swings the view,
  and the game's head bob is gone (one switch in F11 > Comfort).
- **Smooth walking and running:** movement follows the headset's own timing instead of the game's uneven frames.
  No more judder going uphill.
- **Detection chevrons** (when an enemy or animal notices you) are now small, sit on the enemy in both eyes, and no
  longer fill your view.
- **Big world markers** (placed C4, some enemy marks) are kept to a sensible size.
- **"VR mod loading" popup:** after each load, a small popup in the headset holds your controls for a few seconds
  while the mod records your weapons, so the first grabs work. The launcher can skip it ("After a load: Skip the
  loading popup").
- **Map and binoculars:** the map stays on its flat screen and works with every stick, trigger and grip; the
  binoculars magnify the view as on a monitor.
- **B closes tutorial pop-ups.**
- **F11 menu tidied up:** every tab is split into sections you open and close, the Tests tab and old research
  options are gone, and a stray click can no longer switch you to AFW.
- **Far detail** (F11 > Display, optional): the game's level of detail as on a flat screen.

## Picture

- **OFXR Bridge's DLSS frame generation** works with the mod.
- **AFW:** trees and grass no longer shimmer, and the weapons on your body and the pouch are drawn for each eye.
- **Lamps and their shadows** are placed correctly in each eye indoors.
- **New test option for foliage:** F11 > Debug > DLSS, "DLSS follows leaves and grass swaying in the wind (test)",
  for less smearing of swaying foliage. Off by default: try it and tell us.

## Important fixes

- **FPS drop that never came back:** a weapon scan that ran every 5 minutes could drop the game to a few fps for
  good. It no longer runs on a timer.
- **Shots no longer held back:** a held trigger now fires within a quarter second even when the game's aim is
  slightly behind the gun.
- **Iron-sight guns** no longer go into the game's aim-down-sights by themselves near your eye, and each gun is held
  by its own axes. Sight lines for 15 more weapons are included.
- **Your settings for the weapon's angle on the controller** are now saved.

## New defaults

- **Weapon pitch on the controller: -5 degrees.**
- **Even frame pacing: off** (it can be turned on in F11 > Comfort).
- **Smooth walking and body-based view: on.**

See [KNOWN_ISSUES.txt](https://github.com/Homvr/FarcryVVR_Release/blob/main/release/KNOWN_ISSUES.txt) before
reporting. When something looks wrong, send what you did, Stereo or AFW, a short video if you can, and
`fc5vr_log.txt` from the game's `bin` folder (or the launcher's report zip).
