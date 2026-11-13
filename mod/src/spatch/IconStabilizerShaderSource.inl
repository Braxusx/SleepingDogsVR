#pragma once

// Icon stabilizer - embedded shader source.
// A deliberately small custom pass (no FXAA code): it runs after the SMAA
// neighborhood blend and applies a mild 3x3 tent filter ONLY to pixels
// whose local one-texel luma contrast exceeds a gate. The gate confines the
// smoothing to the highest-frequency band of the frame - the small pulsing
// world icons and mission markers whose per-frame edge shimmer the game's
// stock FXAA used to hide - while flat regions and every blend SMAA itself
// produced pass through unchanged. This is the non-FXAA alternative to the
// FXAA assist: no second full-frame AA smoothing, and the touched band is
// far narrower than what FXAA's own early-exit leaves for processing.
//
// The pass reads the blended frame through the linear SRV of the
// sRGB-viewed intermediate, so luma is measured on
// stored gamma values - the space the flicker is perceptually defined in.

#include <array>
#include <string_view>

namespace spatch::smaa {

inline constexpr std::array<std::string_view, 1> kStabilizerShaderSourceChunks = {
    R"__SPATCH_STAB__(
cbuffer SMAAConstants : register(b0) {
    float4 rtMetrics;
    float4 subsampleIndices;
};

Texture2D colorTex : register(t0);

#define STAB_CONTRAST_GATE 0.10
#define STAB_STRENGTH 0.5

float3 StabilizerFetch(int2 p) {
    const int2 size = int2(max(rtMetrics.zw, float2(1.0, 1.0)));
    return colorTex.Load(int3(clamp(p, int2(0, 0), size - 1), 0)).rgb;
}

float StabilizerLuma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float4 IconStabilizerPS(float4 svpos : SV_POSITION) : SV_TARGET {
    const int2 p = int2(svpos.xy);
    const float3 center = StabilizerFetch(p);
    const float lumaC = StabilizerLuma(center);

    float lumaMin = lumaC;
    float lumaMax = lumaC;
    float3 axisSum = float3(0.0, 0.0, 0.0);
    float3 diagSum = float3(0.0, 0.0, 0.0);
    const int2 kAxis[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
    const int2 kDiag[4] = {int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1)};
    [unroll] for (int i = 0; i < 4; ++i) {
        const float3 a = StabilizerFetch(p + kAxis[i]);
        axisSum += a;
        const float la = StabilizerLuma(a);
        lumaMin = min(lumaMin, la);
        lumaMax = max(lumaMax, la);
        const float3 d = StabilizerFetch(p + kDiag[i]);
        diagSum += d;
        const float ld = StabilizerLuma(d);
        lumaMin = min(lumaMin, ld);
        lumaMax = max(lumaMax, ld);
    }

    const float contrast = lumaMax - lumaMin;
    if (contrast < STAB_CONTRAST_GATE) {
        return float4(center, 1.0);
    }

    const float strength = STAB_STRENGTH *
        saturate((contrast - STAB_CONTRAST_GATE) / 0.15);
    const float3 tent = (center * 4.0 + axisSum * 2.0 + diagSum) / 16.0;
    return float4(lerp(center, tent, strength), 1.0);
}
)__SPATCH_STAB__",
};

}  // namespace spatch::smaa
