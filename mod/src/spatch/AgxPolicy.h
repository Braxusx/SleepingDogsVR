#pragma once

// Pure policy layer for the native SPatch AgX tonemapping port. This header
// deliberately has no D3D11 dependency so the SPatchTests target (which links
// no graphics libraries) can compile and exercise every rule: the stock
// final pre-HUD pixel-shader identity contract, the configuration clamps,
// the INI alias mapping, the look-name parser, and the compile-variant
// selector shared with the runtime.

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace spatch::agx {

// ---------------------------------------------------------------------------
// Stock shader identity (captured from the verified final pre-HUD boundary).
//
// Sleeping Dogs reconstructs scene-linear color in one exact pixel shader
// immediately before the HUD. Substituting that exact shader is the only
// observed boundary that covers the complete scene without processing the
// HUD. All three identity signals must hold before any substitution is
// armed; any mismatch leaves the native shader active (fail-open).
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kFinalPreHudPixelShaderHash = 0x67843125;
inline constexpr std::size_t kFinalPreHudPixelShaderSize = 1852;
inline constexpr std::array<std::uint8_t, 16> kFinalPreHudDxbcChecksum = {
    0x08, 0xCD, 0x71, 0xA4, 0x19, 0x9D, 0x73, 0xDF,
    0x31, 0x4A, 0x99, 0xE7, 0x7C, 0x92, 0x3E, 0xCA,
};

// The DXBC container starts with the fourcc "DXBC" followed by a 16-byte
// checksum of everything after it. Both the size and the embedded checksum
// are required; the CRC32 is a third, whole-blob confirmation.
[[nodiscard]] inline bool HasDxbcSignature(const std::uint8_t* bytes,
                                            std::size_t size) noexcept {
    return bytes != nullptr && size >= 20 &&
        std::memcmp(bytes, "DXBC", 4) == 0 &&
        std::memcmp(bytes + 4,
                    kFinalPreHudDxbcChecksum.data(),
                    kFinalPreHudDxbcChecksum.size()) == 0;
}

// CRC32 (Gary S. Brown table, polynomial 0xEDB88320, reflected, inverted
// output) matching the reference implementation used to capture the stock
// shader hash. Reimplemented here from the public-domain table algorithm so
// SPatch does not depend on ReShade's example headers.
[[nodiscard]] inline std::uint32_t Crc32(const std::uint8_t* bytes,
                                         std::size_t size) noexcept {
    struct Table {
        std::array<std::uint32_t, 256> values{};
        constexpr Table() {
            for (std::uint32_t index = 0; index < 256; ++index) {
                std::uint32_t entry = index;
                for (int bit = 0; bit < 8; ++bit) {
                    entry = (entry & 1) != 0
                        ? (entry >> 1) ^ 0xEDB88320u
                        : entry >> 1;
                }
                values[index] = entry;
            }
        }
    };
    static constexpr Table table{};
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t index = 0; index < size; ++index) {
        crc = (crc >> 8) ^ table.values[(crc ^ bytes[index]) & 0xFFu];
    }
    return ~crc;
}

// Exact identity contract: size, DXBC signature, embedded checksum, and the
// whole-blob CRC32 must all match. This mirrors the proven ShenLong
// HasExpectedDxbcIdentity semantics at the native port boundary.
[[nodiscard]] inline bool HasExpectedDxbcIdentity(const std::uint8_t* bytes,
                                                  std::size_t size) noexcept {
    if (bytes == nullptr || size != kFinalPreHudPixelShaderSize) {
        return false;
    }
    if (!HasDxbcSignature(bytes, size)) {
        return false;
    }
    return Crc32(bytes, size) == kFinalPreHudPixelShaderHash;
}

// A same-size blob whose embedded checksum and CRC disagree is a near-match:
// evidence that a different build or a mutated shader reached the boundary.
// Refusing it (rather than silently falling back) keeps the failure visible.
[[nodiscard]] inline bool LooksLikeMismatchedTarget(const std::uint8_t* bytes,
                                                    std::size_t size) noexcept {
    if (bytes == nullptr || size != kFinalPreHudPixelShaderSize) {
        return false;
    }
    const bool checksum_matches = HasDxbcSignature(bytes, size);
    const bool crc_matches = Crc32(bytes, size) == kFinalPreHudPixelShaderHash;
    return checksum_matches != crc_matches;
}

// ---------------------------------------------------------------------------
// Configuration semantics (percent-based, matching the shipped INI).
// ---------------------------------------------------------------------------

inline constexpr int kAgxStrengthDefaultPercent = 100;
inline constexpr int kAgxStrengthMinPercent = 0;
inline constexpr int kAgxStrengthMaxPercent = 100;
inline constexpr int kAgxExposureDefaultPercent = 100;
inline constexpr int kAgxExposureMinPercent = 25;
inline constexpr int kAgxExposureMaxPercent = 400;

// Look selection: 0 = Neutral (upstream base AgX), 1 = MediumHigh (the
// stock-matched toe-preserving grade; not an official AgX preset).
inline constexpr int kAgxLookNeutral = 0;
inline constexpr int kAgxLookMediumHigh = 1;
inline constexpr int kAgxLookDefault = kAgxLookMediumHigh;

// Diagnostic only: the AgXDebugBloom compile variant. 0 is the shipped image;
// 1, 2 and 3 each REPLACE the whole frame with a view of the engine's
// texHDRBloom (see AgxShaderSource.inl). Nothing is suppressed and nothing is
// fixed. Because it replaces the frame, a debug run and an ordinary run are
// two separate launches and their observations must not be combined.
inline constexpr int kAgxDebugBloomMin = 0;
inline constexpr int kAgxDebugBloomMax = 3;
inline constexpr int kAgxDebugBloomDefault = kAgxDebugBloomMin;

[[nodiscard]] inline constexpr int ClampAgxDebugBloom(int value) noexcept {
    return value < kAgxDebugBloomMin
               ? kAgxDebugBloomMin
               : (value > kAgxDebugBloomMax ? kAgxDebugBloomMax : value);
}

[[nodiscard]] inline int ClampAgxStrengthPercent(int value) noexcept {
    if (value < kAgxStrengthMinPercent) {
        return kAgxStrengthMinPercent;
    }
    return value > kAgxStrengthMaxPercent ? kAgxStrengthMaxPercent : value;
}

[[nodiscard]] inline int ClampAgxExposurePercent(int value) noexcept {
    if (value < kAgxExposureMinPercent) {
        return kAgxExposureMinPercent;
    }
    return value > kAgxExposureMaxPercent ? kAgxExposureMaxPercent : value;
}

[[nodiscard]] inline constexpr int ClampAgxLook(int value) noexcept {
    return value == kAgxLookNeutral ? kAgxLookNeutral : kAgxLookMediumHigh;
}

// ---------------------------------------------------------------------------
// Look-name parsing. Accepts the canonical PascalCase names, the spaced
// "Medium High" spelling, and the numeric forms; anything else falls back.
// ---------------------------------------------------------------------------

[[nodiscard]] inline bool EqualsCaseInsensitive(std::wstring_view left,
                                                std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        wchar_t a = left[index];
        wchar_t b = right[index];
        if (a >= L'A' && a <= L'Z') {
            a = static_cast<wchar_t>(a - L'A' + L'a');
        }
        if (b >= L'A' && b <= L'Z') {
            b = static_cast<wchar_t>(b - L'A' + L'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline int ParseLookName(std::wstring_view value,
                                        int fallback) noexcept {
    if (value.empty()) {
        return fallback;
    }
    if (EqualsCaseInsensitive(value, L"Neutral") || value == L"0") {
        return kAgxLookNeutral;
    }
    if (EqualsCaseInsensitive(value, L"MediumHigh") ||
        EqualsCaseInsensitive(value, L"Medium High") || value == L"1") {
        return kAgxLookMediumHigh;
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// Compile-variant selection.
//
// The replacement HLSL is fully macro-driven: SPATCH_AGX_EXPOSURE,
// SPATCH_AGX_STRENGTH, SPATCH_AGX_FULL_STRENGTH, SPATCH_AGX_LOOK and
// SPATCH_AGX_DEBUG_BLOOM are consumed by the preprocessor only (there is no
// runtime cbuffer slot for them - register b0 carries the engine's own
// cbShaderParams). Exactly one variant is therefore compiled at startup from
// the clamped configuration, matching the proven ShenLong mechanism. Config
// changes require a game restart, like every other SPatch feature.
//
// SPATCH_AGX_DEBUG_BLOOM is a DIAGNOSTIC, not a fix: at 0 it contributes no
// code at all and the compiled shader is unchanged.
// ---------------------------------------------------------------------------

struct CompileVariant {
    double exposure = 1.0;
    double strength = 1.0;
    bool full_strength = true;
    int look = kAgxLookDefault;
    int debug_bloom = kAgxDebugBloomDefault;
};

[[nodiscard]] inline CompileVariant SelectCompileVariant(
    int strength_percent,
    int exposure_percent,
    int look,
    int debug_bloom = kAgxDebugBloomDefault) noexcept {
    CompileVariant variant{};
    variant.strength =
        static_cast<double>(ClampAgxStrengthPercent(strength_percent)) / 100.0;
    variant.exposure =
        static_cast<double>(ClampAgxExposurePercent(exposure_percent)) / 100.0;
    variant.full_strength = variant.strength >= 1.0;
    variant.look = ClampAgxLook(look);
    variant.debug_bloom = ClampAgxDebugBloom(debug_bloom);
    return variant;
}

}  // namespace spatch::agx
