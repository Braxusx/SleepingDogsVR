# Changelog

## 0.8.8

- **Fix for double vision on Meta's own VR runtime (Meta Quest Link / the Oculus app).** The 3D world appeared doubled
  while the menus looked right. The mod drew each eye with a wider view than the headset's own and told the runtime which
  angles the picture covers; Virtual Desktop and SteamVR respect that, Meta's runtime does not. On Meta's runtime the mod
  now hands over only the part of each picture that lies inside the headset's own view. Nothing visible is lost.
  `SubmitFov` in `sdvr.ini` can force this on (1) or off (2) on any runtime; 0 is automatic.
- **Snap turning.** Launcher: Settings > View > *Snap turning*, or the in-game VR menu (Insert) > *SNAP TURN*: off, 15, 30
  or 45 degrees. A flick of the right stick turns the view by one step at once; holding it gives a further step every half
  second. It works behind Wei and in first person. While the left trigger is held (aiming) the stick turns smoothly as
  before. If the game's camera does not follow the stick (some minigames and scripted moments) the stick is handed back
  to the game for a few seconds.
- **First person: cutscenes keep the game's camera.** With first person switched on, the game's own camera is used for
  as long as a cutscene plays, and first person comes back when it ends.
- Mod build 146.

## 0.8.7

Plays the same as 0.8.6. This release adds diagnostics for picture problems (double vision, the two eyes not matching,
something stray on the HUD panel), which leave nothing in the log.

- **View report in the log.** When tracking starts, each time the headset session starts again, and when the headset's eye
  spacing or field of view changes, the log records what the VR runtime reports for the two eyes (spacing, direction, field
  of view), what the game drew for them, and what was handed to the headset (eye pictures, HUD panel, menu screen).
- **Picture check.** About > *Record a picture check* in the launcher, then play for at least 20 seconds. The mod saves one
  small picture of each eye and of the HUD panel, once (`sdvr_check_*.bmp` in the game folder). *Collect logs* gathers them.
- The log also counts what was handed to the headset over time: stereo world, menu screen, HUD panel, frames without a
  matched head pose.
- Mod build 144.

**Reporting a picture problem:** About > Record a picture check, play 20 seconds, About > Collect logs, and send the zip.
Please also say what is doubled (everything, only near things, only the HUD or menus, only moving things), and whether it
looks right with one eye closed.

## 0.8.6

- **Fix for a crash inside the graphics driver**, reported with Virtual Desktop in VDXR mode (the game closing during play,
  sometimes within the first minute). The mod's graphics hooks also ran for drawing done by other components inside the game
  process, such as the VR runtime's own thread, and treated it as the game's. They now act only on the game's own drawing on
  the game's render thread. This may also be the cause of rare mid-game crashes on other set-ups.
- The crash diagnostics of 0.8.5 stay in (state file, previous run kept, freeze and exit records). The log now also names any
  other component whose drawing passes through the hooks.
- Mod build 143.

**Reporting a crash:** About > Collect logs in the launcher, and send the zip.

## 0.8.5 - diagnostic build

Plays the same as 0.8.4. This release adds diagnostics for crashes and freezes that leave nothing in the log (reported with
Virtual Desktop in VDXR mode). If you have no crashes you do not need it, but it does no harm.

- **State file.** `sdvr_state.txt` in the game folder always shows what the mod was doing last: the OpenXR call each thread
  was inside, the frame, memory use, the last exception. It survives every kind of crash.
- **The previous run is kept.** When the game starts, the last run's files are kept as `sdvr_previous.log` and
  `sdvr_previous_state.txt`, so starting the game again after a crash no longer wipes the evidence.
- **Freezes are recorded.** When the game stops producing frames the log gets a `STALL` entry with what the render thread and
  the game thread were doing.
- **More exits are recorded:** abort and fail-fast exits, a lost graphics device (with the reason), and C++ exceptions with
  their class name and message.
- **Machine details in the log:** graphics card and driver, Windows build, processor, memory use once a minute, a warning when
  graphics memory is full, and modules that other programs loaded into the game (overlays, capture tools).
- **Launcher:** marked as a diagnostic build; *Collect logs* gathers the new files; uninstall removes them.
- Mod build 142.

**Reporting a crash:** About > Collect logs in the launcher, and send the zip. Please also say whether the game froze or
vanished and what you were doing.

## 0.8.4

- **Gamepad shortcuts are yours to choose.** In the VR menu (Insert) the new page *Gamepad shortcuts* lists two buttons each
  for first person on / off and for recentre. Choose a line, press Right, then press the button you want on the gamepad.
  Triggers and stick clicks can be used; a shortcut can also be a single button. The hold time is adjustable. Defaults are
  unchanged (LB + B, LB + Y). New `sdvr.ini` keys: `PadFirstPersonButton1/2`, `PadRecenterButton1/2`.
- **See-through VR menu.** The menu and messages in the headset are drawn semi-transparent.
- **Launcher:** Settings now has a guide to the in-game VR menu (how to use it and what each line does) in place of the
  gamepad shortcut switches.
- Mod build 141.

## 0.8.0 - 2026-11-13

First public release. A virtual reality mod for **Sleeping Dogs: Definitive Edition** (Steam or GOG).

### Features
- The whole game in native stereo 3D with head tracking and leaning
- NVIDIA DLSS / DLAA with selectable presets, plus sharpening
- HUD on a floating panel with the minimap and objective markers
- Menus, the map and cutscene subtitles on a screen in front of you
- Shadows, driving mist, lights and scenery fixed for both eyes
- Depth of field / speed blur, the red hurt overlay and lens water drops are off by default and optional; bloom can be switched off
- World scale slider and an **experimental** first person view
- In-game VR menu (Insert)
- Launcher: install / uninstall, game settings check, all options, log collection
- **SPatch by xowny** built in

### Requirements
- Sleeping Dogs: Definitive Edition, current Steam or GOG release
- A PC VR headset with an OpenXR runtime (Virtual Desktop, SteamVR, Meta Quest Link ...)
- NVIDIA RTX graphics card for DLSS (optional)

### Install
1. Download `SleepingDogsVR-Launcher.exe` from the release and run it.
2. Press **Install VR mod**, then **Fix game settings** if the launcher asks.
3. Start your headset's PC app, then press **PLAY**.

### Controls
- Hold LB + hold Y: recentre the view
- Hold LB + hold B: first person on / off
- Use these two on foot only. While driving, Y gets you out of the vehicle and B is the brake.
- Keyboard: Insert opens the VR menu, F8 recentres, F11 changes the HUD mode, F7 the monitor window eye

### Known issues
- With Virtual Desktop, some setups show a brief hitch about every 20 seconds, most noticeable in cutscenes. The mod's log shows
  the frame waiting in the hand-off to the streamer; this is being looked into.
- First person view is experimental.

### Credits
- **SPatch by xowny**, built in with his permission.
  [Nexus Mods](https://www.nexusmods.com/sleepingdogsdefinitiveedition/mods/148) - [GitHub](https://github.com/xowny/SPatch)
- NVIDIA DLSS (C) NVIDIA Corporation. MinHook, OpenXR, Dear ImGui.

### Licence
GNU GPL v3, with an additional permission to link NVIDIA's DLSS SDK. `nvngx_dlss.dll` inside the launcher is proprietary NVIDIA
software under the NVIDIA RTX SDKs License and is not covered by the GPL.
