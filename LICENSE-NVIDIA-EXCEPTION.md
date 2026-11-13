# Additional permission for NVIDIA DLSS (GNU GPL version 3, section 7)

Sleeping Dogs VR is licensed under the GNU General Public License version 3 (see `LICENSE`).

As an additional permission under section 7 of the GNU General Public License version 3, the copyright holder of
Sleeping Dogs VR (Braxus) gives you permission to link or combine the parts of this program that he holds the copyright
to - the VR mod in `mod/` outside `mod/src/spatch/`, and the launcher in `launcher/` - with NVIDIA's DLSS (NGX) SDK
libraries, including `nvsdk_ngx` and `nvngx_dlss.dll`, and to convey the resulting work. The NVIDIA libraries are
governed by the NVIDIA RTX SDKs License Agreement and not by the GNU General Public License; the Corresponding Source of
the combined work does not need to include their source code.

This permission does not extend to:

- `mod/src/spatch/` (SPatch by xowny, with this mod's changes). It is licensed under the GNU General Public License
  version 3 without any additional permission. It is built as its own library, `sdvr_spatch.dll`, which contains no
  NVIDIA code.
- third-party components, which keep their own licences (see `THIRD_PARTY_NOTICES.md`).

If you modify this program you may extend this permission to your version, but you are not obliged to. If you do not
wish to, delete this file and the reference to it from your version.

NVIDIA's own files are not part of this repository. `nvngx_dlss.dll` is proprietary software of NVIDIA Corporation; it
is shipped only inside the released launcher, together with a copy of the NVIDIA RTX SDKs License and a notice.
