#pragma once

#include <Windows.h>
#include <d3d11.h>

namespace spatch::shadow_scale {

// Private-data GUID stamped onto every scaled shadow-map resource via
// ID3D11DeviceChild::SetPrivateData. The payload is a
// {native_size, scaled_size} pair (2 x uint32). Private data dies with the
// object, so scaled maps carry their own identity: no destroy tracking, no
// registry maps, no stale entries.
// {A1F4B832-9C60-45CE-9D2E-6D5A0E1B7C44}
inline constexpr GUID kScaledShadowMapGuid = {
    0xA1F4B832,
    0x9C60,
    0x45CE,
    {0x9D, 0x2E, 0x6D, 0x5A, 0x0E, 0x1B, 0x7C, 0x44}};

// Called by the integration layer when the game's D3D11 device has been
// captured (SmaaRuntime publishes it after the first successful Present).
// Arms the native CreateTexture2D/CreatePixelShader and context detours
// exactly once, fail-closed: any install failure disables map and filter
// scaling together. Reads [Shadows] ShadowResolution and the [Debug] keys
// from SPatch.ini next to the SPatch module. Later calls are no-ops.
void NoteCapturedDevice(ID3D11Device* device);

// Called once per presented frame (from the Present detour). Drives the
// census snapshot cadence; a no-op unless the CensusShadowConsumers debug
// key is set.
void NoteFrame();

// Called when the D3D11 device is lost or torn down. Closes the behavioral
// gate and makes the retained detours transparent (MinHook trampolines stay
// process-lifetime, like every other SPatch detour).
void NoteDeviceLost();

// Starts the device-creation watch: a short-lived background thread that
// waits for the game's d3d11.dll to load, then detours its
// D3D11CreateDevice / D3D11CreateDeviceAndSwapChain exports so the device
// is captured at creation. This is the primary arm path on installs where
// a third-party dxgi.dll owns Present (the SMAA capture path is
// unavailable there). Idempotent; safe to call once at bootstrap.
void StartDeviceWatch();

// True only when the feature is configured on AND the native hooks armed
// successfully. Fail-closed: never true with mismatched maps and filters.
bool Enabled();

}  // namespace spatch::shadow_scale
