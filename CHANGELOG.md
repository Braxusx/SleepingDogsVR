# Changelog

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
