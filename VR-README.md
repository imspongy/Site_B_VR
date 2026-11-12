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

**Right hand (Anne's arm follows it)**

| Input | Action |
|---|---|
| Grip (hold) | Grab whatever your hand touches; keep holding to carry it. Let go to drop, or swing and let go to throw |
| Trigger (holding) | Fire / use the held item |
| Trigger (empty hand) | Force grab: point at an item, hold trigger, flick your wrist toward you, then squeeze grip to catch |
| Cross / A | Jump |
| Circle / B | Crouch (toggle) |
| Stick left/right | Turn (snap or smooth) |
| Stick click | Toggle run |

**Left hand**

| Input | Action |
|---|---|
| Stick | Walk |
| Stick click | Recenter view |
| Grip (near a held long gun) | Two-handed aim |
| Trigger | Throw (hold to wind up) |
| Square / X | Swap held item with holster |
| Triangle / Y | Pause menu |

**Holsters**: let go of a small item (pistol) at your hip, or a big one (rifle, shotgun) over your shoulder,
to holster it. Squeeze grip at the same spot with an empty hand to draw it. You feel a tick when your
hand is at a holster.

**Menus**: point at the floating screen and pull the trigger to click.

Set `LeftHanded=1` in the settings to swap everything over.

## Settings

`TrespasserVR.ini` is created next to the exe on first run, with a comment on every option. Changes
apply within a couple of seconds, even mid-game, so you can tweak while wearing the headset (e.g. via
the SteamVR desktop view). Highlights:

- `SmoothTurn=1` for smooth turning, `SmoothTurnSpeed` for its speed; `SnapTurnDegrees` for snap.
- `MoveWithHand=1` to walk where your left hand points instead of where you look.
- `GripToggle=1` if you'd rather press grip once to grab and again to drop.
- `HandPitch` / `HandYaw` / `HandRoll` to adjust how guns sit in your hand.
- `Haptics` to change vibration strength.

Render resolution is the `Width`/`Height` in `OpenTrespasser.ini`. A square-ish size such as
1024x1024 suits headsets best. Bigger looks sharper but costs frame rate (the game renders twice).

## Known limitations

- The game renders on the CPU and copies each frame to the headset, so frame rates are modest.
  SteamVR motion smoothing helps.
- Walking around your room moves your head up to 0.6 m (`LeanRadius`) from your body; beyond that the
  view stops following. Use the stick to walk.
- Anne has one arm, so only one hand is visible; in left-handed mode it is still her right arm.
