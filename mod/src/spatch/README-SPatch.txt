# SPatch

SPatch is an ASI mod for **Sleeping Dogs: Definitive Edition**.

License: GPL v3

This repository contains SPatch source code only.

- Nexus Mods: https://www.nexusmods.com/sleepingdogsdefinitiveedition/mods/148?tab=description

## Install

SPatch requires a compatible x64 ASI loader beside `sdhdship.exe`. An x86
loader cannot load the patch. The supported executable identities are:

- legacy full feature build: `C6DB199B7692D24231C216FC29DC430EC3AFD59435AD5C1AC589934BE8CC6035`;
- latest Steam build: `2A33EC787AC6FD4C86FEC2B6F778FEEA881A3F35EA56C680121F53571C0527DA`.

Extract the contents of `SPatch-Base.zip` beside `sdhdship.exe`. Keep
`SPatch.asi` and `SPatch.ini` together. The latest Steam compatibility profile
disables features whose runtime paths are not fully mapped.

To uninstall SPatch, remove `SPatch.asi`. Remove `SPatch.ini`, `SPatch.log`,
and `SPatch-*.dmp` only when their settings and diagnostics are no longer
needed. Configuration migration backups live under
`%LOCALAPPDATA%\SPatch\ConfigBackups`.

## Included tweaks

- cutscene and NIS timing recovery for arbitrary configured cadences;
- native AgX tonemapping with selectable looks;
- custom frame limiter with a configurable cap;
- display setting persistence and removal of the hidden 120 FPS wait;
- original version fixes for fog, eye material, wetness, sweat, shadow filter, and VRAM;
- optional shadow map resolution upscale;
- guards for files, archives, QCMP, compressed XML, resource streams, and save safety;
- pool diagnostics and a configurable pool headroom;
- native Windows Raw Input, controller deadzones, camera recentering while on foot, and optional vehicle cameras;
- cop radar, heat meter, speed rumble, depth of field, and stunt presentation options;
- restored Wei, NPC, police pursuit, and push voice lines;
- shadows for vehicle headlights and siren lights;
- native texture filtering controls and canonical SMAA 1x;
- optional file logging and bounded crash minidumps.

## Configuration

`SPatch.ini` is generated beside `SPatch.asi` and uses grouped PascalCase
settings. Legacy snake case names remain readable during migration. Common
sections include:

- `[SPatch]` for general toggles;
- `[Cutscenes]` for cadence and scene time recovery;
- `[Display]` for initial resolution, refresh behavior, and the frame limiter;
- `[Gameplay]` for voice lines, dialogue, and radar options;
- `[Graphics]` for restoration fixes and presentation options;
- `[Input]` for raw mouse input, controller deadzones, and vehicle cameras;
- `[TextureFiltering]` for anisotropic filtering tweaks;
- `[AntiAliasing]` for SMAA;
- `[Tonemapping]` for AgX;
- `[Stability]` for file, resource, save, and thread guards;
- `[Debug]` for local logging and crash dumps.

Set `[Debug] Logging=1` to write `SPatch.log`. Set
`[Debug] WriteCrashDumps=1` to write a small timestamped dump when the game
crashes. Diagnostics stay local; SPatch exports no telemetry.

## Build and test

From the repository root:

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
    -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1

& $msbuild .\SPatchTests.vcxproj /t:Build /p:Configuration=Release `
    /p:Platform=x64 /p:FinalRelease=false
.\build\tests\Release\false\SPatchTests.exe

& $msbuild .\SPatch.vcxproj /t:Rebuild /p:Configuration=Release `
    /p:Platform=x64 /p:FinalRelease=true
```

The build resolves pinned `MinHook.h` and `MinHook.lib` from the
`SDmodding/SDK` repository through `tools\Resolve-MinHook.ps1`, and validates
the pinned SMAA sources during the build. Normal builds write only under
`build\`; release packaging writes under `artifacts\`.
