# Sleeping Dogs VR

A virtual reality mod for **Sleeping Dogs: Definitive Edition** by Braxus. The whole game in native stereo 3D with head
tracking, best played seated with a gamepad.

### Features
- Full native stereo rendering of the game's third-person view, with head tracking and leaning
- NVIDIA DLSS / DLAA and sharpening
- HUD on a floating panel (fixed, head-following or hidden) with the minimap and objective markers
- Menus, the map, logo screens and cutscene subtitles on a screen in front of you
- VR fixes: shadows all round you, driving mist in both eyes, lights and scenery matching in both eyes
- Screen effects that don't suit VR are off by default and optional: depth of field / speed blur, red hurt edges,
  water drops on the lens; bloom can be switched off
- World scale slider and an **experimental** first person view
- In-game VR menu (Insert)
- **SPatch by xowny** built in (see Credits)

### Requirements
- Sleeping Dogs: Definitive Edition, Steam or GOG (current release)
- A PC VR headset with an OpenXR runtime (Virtual Desktop, SteamVR, Meta Quest Link ...)
- NVIDIA RTX graphics card for DLSS (optional)

### Install
1. Run `SleepingDogsVR-Launcher.exe`. It finds the game (or use Browse).
2. Press **Install VR mod**, then **Fix game settings** if the launcher asks for it.
3. Start your headset's PC app, then press **PLAY**.

Uninstall from the same launcher; your save games are not touched.

### Controls
| | |
|---|---|
| Hold LB + hold Y | Recentre the view |
| Hold LB + hold B | First person on / off |
| Insert | VR menu (arrow keys select and change) |
| F8 | Recentre |
| F11 | HUD panel mode |
| F7 | Monitor window eye |

Use the two gamepad shortcuts on foot only - before getting into a vehicle or onto a motorcycle, or after getting out.
While driving, Y gets you out of the vehicle and B is the brake. F8 and the Insert menu work at any time.

### Building from source
- `mod/` - the VR mod. `mod/build.sh <number>` builds `dinput8.dll` and `sdvr_spatch.dll` with llvm-mingw (x86_64). The DLSS part
  needs NVIDIA's NGX SDK (headers and static library, from https://github.com/NVIDIA/DLSS), which is not in this repository;
  set `NGX` to where you put it.
- `launcher/` - the launcher. Put the built `dinput8.dll` and `sdvr_spatch.dll` and NVIDIA's `nvngx_dlss.dll` into
  `launcher/payload/`, then run `launcher/build.sh` (llvm-mingw, 32-bit).
- `mod/src/spatch/` is the complete source of `sdvr_spatch.dll` (SPatch with this mod's changes).

### Licence
Sleeping Dogs VR is free software under the **GNU General Public License v3** (see `LICENSE`).

- The author's own code (the VR mod outside `mod/src/spatch/`, and the launcher) comes with an additional permission to
  link it with NVIDIA's DLSS SDK - see `LICENSE-NVIDIA-EXCEPTION.md`.
- `mod/src/spatch/` is SPatch by xowny with this mod's changes: GPL v3, no additional permission. It is built as its own
  library, `sdvr_spatch.dll`.
- NVIDIA's `nvngx_dlss.dll` is proprietary NVIDIA software and is **not** covered by the GPL. It is not in this
  repository; the released launcher carries it under the NVIDIA RTX SDKs License, with a copy of that licence.
- Third-party parts keep their own licences: `THIRD_PARTY_NOTICES.md`.

### Credits
- Sleeping Dogs VR by Braxus.
- **SPatch by xowny** - a fix and restoration patch for Sleeping Dogs: Definitive Edition, built into this mod as
  `sdvr_spatch.dll` with the author's permission. Thank you, xowny.
  [Nexus Mods](https://www.nexusmods.com/sleepingdogsdefinitiveedition/mods/148) - [GitHub](https://github.com/xowny/SPatch)
  SPatch is licensed under the GNU GPL v3; `sdvr_spatch.dll` is distributed under the same licence and its complete source,
  with this mod's changes, is in this repository (`mod/src/spatch`).
- NVIDIA DLSS (C) NVIDIA Corporation. MinHook by Tsuda Kageyu. OpenXR by the Khronos Group. Dear ImGui by Omar Cornut.
- Launcher built on the Far Cry 2 VR launcher, whose design follows Pizza Parker's Dishonored VR launcher (with permission).

Sleeping Dogs is a trademark of Square Enix. This is an unofficial fan mod, not affiliated with or endorsed by Square Enix
or United Front Games.
