#pragma once

#include <Windows.h>
#include <dxgi.h>

#include <cstdint>

struct ID3D11PixelShader;

namespace spatch::agx {

// Native SPatch AgX tonemapping boundary. The replacement substitutes only
// the exact known final pre-HUD pixel shader (identity contract in
// AgxPolicy.h); every other shader passes through untouched. All failure
// paths leave the stock tone mapper active.

// Publishes the engine D3D11 device slot once Hooks has resolved it. The
// slot is read with AddRef'd retention inside the runtime; a null slot
// simply keeps AgX inactive (fail-open).
void SetDeviceSlot(std::uintptr_t device_slot);

// Applies the clamped configuration (percent scales, look, diagnostic mode).
// Called once from BootstrapMain after LoadConfig; a restart applies changes.
//
// `debug_bloom` selects the AgxDebugBloom DIAGNOSTIC compile variant (0 = the
// shipped image; 1-3 each REPLACE the whole frame with a view of the engine's
// texHDRBloom). It suppresses nothing and fixes nothing. Because it replaces
// the frame, a debug run and an ordinary run are two separate launches and
// their observations must not be combined; the modes are #if/#elif on a macro
// baked in at shader compile time, so a launch is exactly one mode.
//
// Read mode 1 (raw bloom colour), not mode 2. Mode 2's chroma-spread scalar is
// 0 for any achromatic bloom and non-zero for a weakly saturated one, so its
// flat look means two opposite things: flat black = the bloom is the drain;
// flat dark grey = the bloom is exonerated and the drain is downstream.
void Configure(bool enabled,
               int strength_percent,
               int exposure_percent,
               int look,
               int debug_bloom = 0);

// Per-frame lazy hook installer, modeled on smaa::MaybeInstallHooks. Retains
// a ComPtr to the device from the published slot, reads the
// ID3D11Device::CreatePixelShader vtable entry (slot 15), verifies the
// target with the same hook_guard used by SMAA, and installs the MinHook
// detour exactly once. Never installs when AgX is disabled - the bundled
// MinHook fork frees trampolines on removal, so the detour is retained for
// process lifetime once installed (same doctrine as SMAA).
void MaybeInstallHooks();

}  // namespace spatch::agx
