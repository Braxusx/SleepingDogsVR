# Third-party components

| Component | Where | License |
|---|---|---|
| SPatch (xowny) - https://github.com/xowny/SPatch | `mod/src/spatch` (built as `sdvr_spatch.dll`, with this mod's changes) | GPL-3.0 (see `mod/src/spatch/LICENSE-GPL-3.0.txt`) |
| SMAA (Jorge Jimenez and others), part of SPatch | `mod/src/spatch/third_party/smaa` | MIT (see `LICENSE.txt` there) |
| MinHook (Tsuda Kageyu) | `mod/third_party/minhook` | BSD 2-Clause (see `LICENSE.txt` there) |
| OpenXR SDK headers (The Khronos Group) | `mod/third_party/openxr` | Apache-2.0 OR MIT |
| Dear ImGui (Omar Cornut) | `launcher/third_party/imgui` | MIT (see `LICENSE.txt` there) |
| LZMA SDK decoder (Igor Pavlov) | `launcher/src/lzma` | Public domain |
| miniz (Rich Geldreich and others) | `launcher/src/miniz` | MIT (see `LICENSE` there) |
| NVIDIA DLSS (NGX) runtime `nvngx_dlss.dll` | shipped in the launcher only, not in this repository | Proprietary - NVIDIA RTX SDKs License (copy: `launcher/payload/NVIDIA-RTX-SDKs-License.txt`); not covered by the GPL |

SPatch is used with its author's permission. Nexus Mods page: https://www.nexusmods.com/sleepingdogsdefinitiveedition/mods/148

NVIDIA, the NVIDIA logo and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and other countries.
This product uses NVIDIA DLSS technology. The NGX SDK itself (headers and static library) is not included; get it from https://github.com/NVIDIA/DLSS.

The launcher is built on the Far Cry 2 VR launcher, whose design follows Pizza Parker's Dishonored VR launcher (with permission).

Sleeping Dogs is a trademark of Square Enix. This is an unofficial fan mod, not affiliated with or endorsed by Square Enix or
United Front Games; no game files are included or modified on disk.
