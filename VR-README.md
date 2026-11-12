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

## Controls (defaults, rebindable in SteamVR > Controller Bindings)

| Action | Index | Touch / Quest | Vive |
|---|---|---|---|
| Move (where you look) | Left stick | Left stick | Left pad |
| Snap turn | Right stick | Right stick | Right pad |
| Toggle run | Right stick click | Right stick click | – |
| Raise arm (it follows your controller) | Right grip | Right grip | Right grip |
| Grab (empty hand) / fire or use (holding) | Right trigger | Right trigger | Right trigger |
| Grab / drop | A | A | Right pad click |
| Jump | B | B | Right menu |
| Crouch | Left grip | Left grip | Left grip |
| Throw (hold to wind up) | Left trigger | Left trigger | Left trigger |
| Stow / swap item | Left A | X | – |
| Pause menu | Left B | Y | Left menu |
| Recenter view | Left stick click | Left stick click | Left pad click |

Menus and loading screens appear on a floating screen; use the mouse on your PC to click them.

## Settings

`TrespasserVR.ini` is created next to the exe on first run:

- `Enabled=0` turns VR off.
- `SnapTurnDegrees=0` switches to smooth turning (`SmoothTurnSpeed` degrees/second).
- `HandPitch`, `HandYaw`, `HandRoll` rotate the in-game hand relative to your controller. If guns
  point too high or low, adjust `HandPitch` in steps of 10.
- `MotionHand=0` goes back to the original mouse-style arm.
- `FlipFovX=1` / `FlipFovY=1`: debug switches in case the image looks doubled or misaligned.

Render resolution is the `Width`/`Height` in `OpenTrespasser.ini`. A square-ish size such as
1024×1024 suits headsets best. Bigger looks sharper but costs frame rate (the game renders twice).

## Known limitations

- The game renders on the CPU and copies each frame to the headset, so frame rates are modest.
  SteamVR motion smoothing helps.
- Walking around your room moves your head up to 0.6 m (`LeanRadius`) from your body; beyond that the
  view stops following. Use the stick to walk.
- One hand only (Trespasser only has one arm).
