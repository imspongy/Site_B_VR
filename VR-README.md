# Trespasser VR

A VR mod for Jurassic Park: Trespasser, built on the OpenTrespasser source code. Works with any
SteamVR headset (Index, Vive, Quest via Link/Air Link/Virtual Desktop, Pico via SteamVR, etc.).

## Install

1. Back up your Trespasser folder (the one with `trespass.exe` and the `data` folder).
2. Copy everything from this zip into that folder.
3. Start SteamVR, then run `OpenTrespasser.exe`.

If no headset is found the game just runs on the monitor. See `TrespasserVR.log` for details.

`OpenTrespasser-Release.exe` is a slower build with extra checks. Use it if the normal exe crashes
and send the log files it writes.

## Controls

Laid out for PSVR2 Sense and Quest/Touch controllers (Index and Vive also have defaults). Everything
can be rebound in SteamVR > Settings > Controllers > Manage Controller Bindings.

**Both hands work the same way** (right controller = Anne's right arm, left controller = her left arm):

| Input | Action |
|---|---|
| Grip (hold) | Grab whatever the hand touches and keep holding it. Let go to drop, or swing and let go to throw |
| Trigger (holding) | Fire / use the held item |
| Trigger (empty hand) | Force grab: point at an item, hold trigger, flick your wrist toward you, then squeeze grip to catch |
| Grip on the other hand's gun | Two-handed grip. Pistols: your hand cups the bottom of the grip. Rifles/shotguns: the gun aims along the line between your hands |
| Hand at hip / over shoulder | Holsters: let go of an item there to holster it, squeeze grip there with an empty hand to draw it |

Held items sit exactly in your hand with no lag, and your hands push and knock things over.

**Sticks and buttons** (swap sides with `LeftHanded=1`):

| Input | Action |
|---|---|
| Left stick | Walk (the further you push, the faster) |
| Left stick click | Sprint (until you let go of the stick) |
| Right stick | Turn (snap or smooth) |
| Right stick click | Recenter view |
| Cross / A | Jump |
| Circle / B | Crouch (toggle) |
| Triangle / Y | Pause menu |

**Menus**: point at the floating screen and pull the trigger to click.

**Walking around your room** moves Anne with you. If she's blocked (a wall, a rock), you can only lean
a short way (`BodyLeash`) past her before the view stops.

## Settings

`TrespasserVR.ini` is created next to the exe on first run, with a comment on every option. Changes
apply within a couple of seconds, even mid-game. A settings file from an older build is replaced with
the new defaults (the old one is kept as `TrespasserVR.ini.old`). Highlights:

- `MoveSpeed` / `RunSpeed` (metres per second) and `MoveAcceleration` (how quickly you speed up/stop).
- `SmoothTurn=1` for smooth turning, `SmoothTurnSpeed` for its speed; `SnapTurnDegrees` for snap.
- `MoveWithHand=1` to walk where your left hand points instead of where you look.
- `GripToggle=1` if you'd rather press grip once to grab and again to drop.
- `HandPitch` / `HandYaw` / `HandRoll` to adjust how guns sit in your hand.
- `SkyMode` (0 solid, 1 gradient, 2 clouds) and `Haptics` (vibration strength).

Render resolution is the `Width`/`Height` in `OpenTrespasser.ini`. A square-ish size such as
1024x1024 suits headsets best. Bigger looks sharper but costs frame rate (the game renders twice).

To watch or record, use SteamVR's "Display VR View" window (capture it with OBS).

## Known limitations

- The game renders on the CPU and copies each frame to the headset, so frame rates are modest.
  SteamVR motion smoothing helps.
- The left arm is a mirrored copy of Anne's right arm.
