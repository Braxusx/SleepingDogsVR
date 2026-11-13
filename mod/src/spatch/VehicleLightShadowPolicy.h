#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace spatch::vehicle_light_shadow {

// Round75 RE: the engine's deferred-light submit selector (FUN_14005d0c0 at
// RVA 0x5D0C0) receives (render object, light record, packed batch entry).
// The packed entry is a 0x10-stride record from the gather table at
// DAT_1421361d8; byte +2 is a flag byte whose bit 2 selects the game's own
// per-light shadow technique family (DRSPOTLIGHTSHADOWS, id 0x70960186,
// verified by a CRC-32/MPEG-2 reimplementation) and byte +3 indexes the
// per-light texture table. The selector tests the flag byte with mask 0x02;
// when that passes, a global query pair decides between the plain and the
// shadow technique pair. Forcing the shadow technique after the stock
// selector keeps every stock bind path except the declined decision.
inline constexpr std::uintptr_t kLightSubmitSelectorRva = 0x0005D0C0;

// First 24 bytes of FUN_14005d0c0 on the researched legacy build (c6db199b...):
// saves rbx/rdi/r14, reserves 0x90 of stack, then `cmp byte [rdx+0x6A], 0`
// (the specular check) and `mov r15, r8` (the packed entry). Any change in
// these bytes means the build layout moved and the hook must not attach.
inline constexpr std::array<std::uint8_t, 24> kLightSubmitSelectorSignature = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC,
    0x90, 0x00, 0x00, 0x00, 0x80, 0x7A, 0x6A, 0x00, 0x4D, 0x8B, 0xF8, 0x48};

// Packed batch entry layout (0x10-byte stride, DAT_1421361d8 table).
inline constexpr std::size_t kPackedEntryStride = 0x10;
inline constexpr std::size_t kPackedFlagByteOffset = 2;
inline constexpr std::size_t kPackedTextureSlotOffset = 3;
inline constexpr std::size_t kPackedScaleOffset = 4;

// Light record offsets (FUN_1400285d0 parser layout, verified in round75).
inline constexpr std::size_t kLightTypeOffset = 0x68;
inline constexpr std::size_t kLightSpecularOffset = 0x6A;
inline constexpr std::size_t kLightRadiusOffset = 0x4C;
inline constexpr std::size_t kLightFovOffset = 0x88;

// Deferred-light render object technique slot (the selector writes the chosen
// technique id here; diagnostic logging reads it back after the call).
inline constexpr std::size_t kRenderTechniqueOffset = 0xA8;

// CRC-32/MPEG-2("DRSPOTLIGHTSHADOWS") - the shadow technique family the
// engine selects when the packed flag byte carries bit 2. Logged only.
inline constexpr std::uint32_t kDeferredShadowTechniqueId = 0x70960186u;

// The selector reads the packed flag byte with mask 0x02 ("shadow
// eligible"): the round75 session census caught 400 world spot lights, all
// carrying flags 0x03, and every one of them already selected the shadow
// technique - so street lamps and other world spots are inside the engine's
// shadow path already. The final shadow decision is a global query pair
// (FUN_14000b140 / FUN_14069f820), not this byte, which is why a light that
// must cast shadows even when the query declines gets the technique forced
// on its render object after the stock selector runs.
inline constexpr std::uint8_t kShadowEligibleMask = 0x02;

// Stock change-detection after the selector picks a technique: the dirty
// flag ushort at *(render_object + 0x78) + 0x78 + render_object, bit 0x20.
inline constexpr std::size_t kRenderDirtyFlagPointerOffset = 0x78;
inline constexpr std::uint16_t kRenderDirtyFlag = 0x20;

// The light-record type byte each configured force matches. Type 5 was the
// static hypothesis for the vehicle family; the first session census never
// saw one inside its budget window, so the default stays 5 but is
// configurable per key without a rebuild.
inline constexpr int kVehicleShadowLightTypeDefault = 5;
inline constexpr int kVehicleShadowLightTypeMin = 0;
inline constexpr int kVehicleShadowLightTypeMax = 255;

[[nodiscard]] inline bool ShouldForceShadow(std::uint8_t light_type,
                                            int configured_type) noexcept {
    return configured_type >= kVehicleShadowLightTypeMin &&
           configured_type <= kVehicleShadowLightTypeMax &&
           light_type == static_cast<std::uint8_t>(configured_type);
}

[[nodiscard]] inline bool ShouldRewriteTechnique(
    std::uint32_t technique) noexcept {
    return technique != kDeferredShadowTechniqueId;
}

}  // namespace spatch::vehicle_light_shadow
