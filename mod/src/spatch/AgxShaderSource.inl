#pragma once

// AgX final pre-HUD replacement shader - embedded HLSL source.
// Ported verbatim from the proven ShenLong integration (the shader text is
// byte-identical; only the embedding host changed from the ReShade add-on
// to SPatch's native D3D11 boundary).
//
// The shader is fully macro-driven: SPATCH_AGX_EXPOSURE,
// SPATCH_AGX_STRENGTH, SPATCH_AGX_FULL_STRENGTH, and SPATCH_AGX_LOOK are
// preprocessor-only inputs (exposure is a multiply at the transform entry,
// strength gates the FULL_STRENGTH specialization and partial-strength
// lerps, look selects the MediumHigh grade via #if). There is no runtime
// cbuffer slot for these - cbuffer b0 carries only the engine's own
// cbShaderParams (white scale, gamma). AgxRuntime therefore compiles
// exactly ONE variant at startup from the clamped configuration, using
// D3D_SHADER_MACRO values formatted "%.9g", FULL_STRENGTH="1" iff strength
// >= 100%, and LOOK "0"/"1". This matches the compile contract proven in
// the ShenLong runtime. A restart applies new settings, like every other
// SPatch feature.
//
// Attribution: the linear-sRGB/Rec.2020 conversions follow three.js (MIT);
// the analytic contrast polynomial follows Filament (Apache-2.0). See
// artifacts/release-notices/THIRD_PARTY_NOTICES.txt and the license files.
// The one extra macro, SPATCH_AGX_DEBUG_BLOOM, is a DIAGNOSTIC and not a fix;
// see the block in main(). At 0 it contributes no code at all, so the emitted
// HLSL is byte-identical to the build without it.

namespace spatch::agx {

inline constexpr char kAgxFinalPreHudShader[] = R"SPATCH_HLSL(
#ifndef SPATCH_AGX_EXPOSURE
#define SPATCH_AGX_EXPOSURE 1.0
#endif

#ifndef SPATCH_AGX_STRENGTH
#define SPATCH_AGX_STRENGTH 1.0
#endif

#ifndef SPATCH_AGX_FULL_STRENGTH
#define SPATCH_AGX_FULL_STRENGTH 1
#endif

#ifndef SPATCH_AGX_LOOK
#define SPATCH_AGX_LOOK 1
#endif

// Diagnostic only. 0 = the shipped image; 1, 2 and 3 each REPLACE the whole
// frame with a view of the engine's texHDRBloom, so the operator sees the
// bloom instead of the game (see main()). Nothing is suppressed, nothing is
// fixed, and the drain being investigated is neither fixed nor disproved.
#ifndef SPATCH_AGX_DEBUG_BLOOM
#define SPATCH_AGX_DEBUG_BLOOM 0
#endif

cbuffer cbShaderParams : register(b0)
{
  struct
  {
    float4 Value0;
    float4 Value1;
    float4 Value2;
    float4 Value3;
    float4 Value4;
    float4 Value5;
    float4 Value6;
    float4 Value7;
  } cbShaderParams : packoffset(c0);
}

SamplerState _texDiffuse_s : register(s0);
SamplerState _texHDRBloom_s : register(s1);
Texture2D<float4> texDiffuse : register(t0);
Texture2D<float4> texHDRBloom : register(t1);

float3 LinearSrgbToLinearRec2020(float3 color)
{
  return float3(
    dot(color, float3(0.6274, 0.3293, 0.0433)),
    dot(color, float3(0.0691, 0.9195, 0.0113)),
    dot(color, float3(0.0164, 0.0880, 0.8956)));
}

float3 LinearRec2020ToLinearSrgb(float3 color)
{
  return float3(
    dot(color, float3(1.6605, -0.5876, -0.0728)),
    dot(color, float3(-0.1246, 1.1329, -0.0083)),
    dot(color, float3(-0.0182, -0.1006, 1.1187)));
}

float3 AgxInset(float3 color)
{
  return float3(
    dot(color, float3(0.856627153315983, 0.0951212405381588, 0.0482516061458583)),
    dot(color, float3(0.137318972929847, 0.761241990602591, 0.101439036467562)),
    dot(color, float3(0.11189821299995, 0.0767994186031903, 0.811302368396859)));
}

float3 AgxOutset(float3 color)
{
  return float3(
    dot(color, float3(1.1271005818144368, -0.11060664309660323, -0.016493938717834573)),
    dot(color, float3(-0.1413297634984383, 1.157823702216272, -0.016493938717834257)),
    dot(color, float3(-0.14132976349843826, -0.11060664309660294, 1.2519364065950405)));
}

float3 AgxDefaultContrastApprox(float3 x)
{
  const float3 x2 = x * x;
  const float3 x4 = x2 * x2;
  const float3 x6 = x4 * x2;
  return -17.86 * x6 * x
       + 78.01 * x6
       - 126.7 * x4 * x
       + 92.06 * x4
       - 28.72 * x2 * x
       + 4.361 * x2
       - 0.1718 * x
       + 0.002857;
}

float3 AgxToLinearSrgb(float3 color)
{
  static const float AgxMinEv = -12.47393;
  static const float AgxMaxEv = 4.026069;
  static const float AgxMiddleGrayLog = 10.0 / 16.5;
  static const float AgxShadowChromaStart = 0.05;
  static const float AgxShadowChromaEnd = 0.20;
  static const float AgxHighlightChromaStart = 0.80;
  static const float AgxHighlightChromaEnd = 0.95;
  static const float AgxMidtoneSaturation = 1.04;
  static const float AgxHighlightSaturation = 1.02;

  color *= SPATCH_AGX_EXPOSURE;
  color = LinearSrgbToLinearRec2020(color);
  color = AgxInset(color);
  color = log2(max(color, 1e-10));
  color = saturate((color - AgxMinEv) / (AgxMaxEv - AgxMinEv));
#if SPATCH_AGX_LOOK == 1
  const float3 contrastBlend =
    smoothstep(0.35, AgxMiddleGrayLog, color);
  const float3 contrast = lerp(1.10, 1.25, contrastBlend);
  color = saturate((color - AgxMiddleGrayLog) * contrast + AgxMiddleGrayLog);
#elif SPATCH_AGX_LOOK != 0
#error Unsupported SPATCH_AGX_LOOK value
#endif
  color = AgxDefaultContrastApprox(color);
#if SPATCH_AGX_LOOK == 1
  const float lookLuminance =
    dot(color, float3(0.2126, 0.7152, 0.0722));
  const float lookPeak = max(color.x, max(color.y, color.z));
  const float shadowWeight =
    smoothstep(AgxShadowChromaStart, AgxShadowChromaEnd, lookPeak);
  const float highlightWeight =
    smoothstep(AgxHighlightChromaStart, AgxHighlightChromaEnd, lookPeak);
  const float shadowToMidtoneSaturation =
    lerp(1.0, AgxMidtoneSaturation, shadowWeight);
  const float lookSaturation =
    lerp(shadowToMidtoneSaturation, AgxHighlightSaturation, highlightWeight);
  color = lookLuminance + lookSaturation * (color - lookLuminance);
#endif
  color = AgxOutset(color);
  color = pow(max(color, 0.0), 2.2);
  color = LinearRec2020ToLinearSrgb(color);
  return saturate(color);
}

float SrgbEncodeChannel(float value)
{
  value = saturate(value);
  return value <= 0.0031308
    ? 12.92 * value
    : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}

float3 AgxToDisplaySrgb(float3 color)
{
  const float3 linearColor = AgxToLinearSrgb(color);
  return float3(
    SrgbEncodeChannel(linearColor.x),
    SrgbEncodeChannel(linearColor.y),
    SrgbEncodeChannel(linearColor.z));
}

float3 LimitDisplayPeak(float3 color)
{
  static const float AgxDisplayPeakLimit = 252.0 / 255.0;
  const float peak = max(color.x, max(color.y, color.z));
  [branch]
  if (peak > AgxDisplayPeakLimit)
  {
    color *= AgxDisplayPeakLimit / peak;
  }
  return color;
}

float FiniteOrZero(float value)
{
  // Shader Model 4 comparisons are false for NaN. The magnitude bound also
  // rejects infinities while retaining the full finite FP16 scene domain.
  return (value == value && abs(value) <= 65504.0) ? value : 0.0;
}

float GameWhiteScale()
{
  // The stock shader divides by this unguarded (DXBC id 15 of the 23-instruction
  // final pre-HUD stream). Clamp rather than discard: substituting 1.0 turns a
  // legitimate large white scale - which a dim scene produces, and the engine
  // builds this value as a curve of render exposure (FUN_140049D30, RVA
  // 0x00049D30) - into a silent no-op. Measured at Value0.y = 1e4 that is the
  // difference between a 0.07 image and an 0.82 one, i.e. more than ten times
  // too bright. See artifacts/reverse-engineering/2026-09-28-stunt-screen-fx-
  // render-path-agx.md section 8.
  return clamp(FiniteOrZero(cbShaderParams.Value0.y), 1e-4, 1e4);
}

float GameGammaExponent()
{
  // Stock applies exp(log(c) * w) unguarded (ids 18-20). The old ceiling of
  // 4.0 silently disabled gamma for any display profile above it. 8.0 is still
  // far beyond any real display gamma while keeping an unbounded exponent -
  // the stock shader's fragility, which is not worth reproducing - from turning
  // a frame all-black or all-white.
  return clamp(FiniteOrZero(cbShaderParams.Value0.w), 1e-2, 8.0);
}

float3 FiniteOrZero(float3 value)
{
  return float3(
    FiniteOrZero(value.x),
    FiniteOrZero(value.y),
    FiniteOrZero(value.z));
}

float3 DecodeSceneEncoding(float3 encodedScene)
{
  // The analytic encoding approaches 1.04 but never reaches it for finite
  // scene radiance. Clamp only values outside that mathematical domain so a
  // corrupt/rounded sample cannot divide by zero or inject NaNs into AgX.
  static const float MaxFiniteSceneEncoding = 1.04 - 1e-5;
  encodedScene = clamp(
    FiniteOrZero(encodedScene), 0.0, MaxFiniteSceneEncoding);
  return min(
    0.2 * encodedScene / max(1.04 - encodedScene, 1e-5), 65504.0);
}

float3 StockToneMap(float3 scene, float gameWhiteScale)
{
  const float3 stockInput = max(scene - 0.004, 0.0);
  return stockInput * (6.2 * stockInput + 0.5)
       / (stockInput * (6.2 * stockInput + 1.7) + 0.06)
       / gameWhiteScale;
}

void main(
  float4 position : SV_Position0,
  float2 texcoord : TEXCOORD0,
  out float4 output : SV_Target0)
{
  // texDiffuse contains the stock scene encoding produced by the earlier
  // scene shaders. This is the exact analytic inverse of 1.04*L/(L+0.2).
  const float3 encodedScene = texDiffuse.Sample(_texDiffuse_s, texcoord).rgb;
  const float4 sampledBloom = texHDRBloom.Sample(_texHDRBloom_s, texcoord);
  const float4 bloom = float4(
    FiniteOrZero(sampledBloom.rgb), FiniteOrZero(sampledBloom.a));
#if SPATCH_AGX_DEBUG_BLOOM != 0
#if SPATCH_AGX_DEBUG_BLOOM < 0 || SPATCH_AGX_DEBUG_BLOOM > 3
#error Unsupported SPATCH_AGX_DEBUG_BLOOM value
#endif
  // DIAGNOSTIC, NOT A FIX. These branches replace the entire frame with a
  // view of texHDRBloom, so the operator can see what the engine feeds the
  // final pre-HUD screen blend. They remove nothing.
  //
  // The early-out sits before DecodeSceneEncoding, before the AgX curve and
  // before the white scale, so a reading is not confounded by the very thing
  // under test. It also means the game image is NOT on screen while this is
  // on: a debug run and an ordinary run are two separate launches, and their
  // observations must not be combined.
#if SPATCH_AGX_DEBUG_BLOOM == 1
  // Raw bloom COLOUR. A grey or near-white bloom during a stunt is the
  // drain: the screen blend below moves the whole frame toward it.
  output = float4(bloom.rgb, 1.0);
  return;
#elif SPATCH_AGX_DEBUG_BLOOM == 2
  // Bloom chroma spread as one greyscale scalar per pixel. AMBIGUOUS BY
  // CONSTRUCTION, and not the mode to read first: `spread` is 0 for ANY
  // achromatic bloom and a non-zero constant for a weakly saturated one, so
  // a flat screen means two opposite things. Flat black = the bloom is
  // achromatic and IS the drain. Flat dark grey = the bloom still has chroma
  // and is exonerated, with the drain downstream of the extract. Use mode 1,
  // which writes the bloom's own RGB and has no threshold to judge.
  const float bhi = max(bloom.r, max(bloom.g, bloom.b));
  const float blo = min(bloom.r, min(bloom.g, bloom.b));
  const float spread = bhi > 1e-4 ? (bhi - blo) / bhi : 0.0;
  output = float4(spread, spread, spread, 1.0);
  return;
#else
  // The .a HDR term in isolation. Note the sign: higher means MORE chroma in
  // the final image, not less.
  output = float4(bloom.a, bloom.a, bloom.a, 1.0);
  return;
#endif
#endif
  const float3 linearScene = DecodeSceneEncoding(encodedScene);
  const float3 exposedScene = min(max(
    linearScene * (1.0 + bloom.a * (linearScene - 1.0)), 0.0), 65504.0);

  // Preserve the engine's dynamic final-composition white scale. Omitting
  // this divisor put AgX in a darker display domain than the stock curve and
  // made both the full-strength result and partial-strength blends washed out.
  const float gameWhiteScale = GameWhiteScale();
  const float3 agx =
    AgxToDisplaySrgb(max(exposedScene, 0.0)) / gameWhiteScale;
#if SPATCH_AGX_FULL_STRENGTH
  const float3 mapped = agx;
#else
  const float3 stock = StockToneMap(exposedScene, gameWhiteScale);
  const float3 mapped = lerp(stock, agx, SPATCH_AGX_STRENGTH);
#endif

  // Preserve the native display-referred bloom screen blend and the game's
  // final user-gamma exponent. The HUD is rendered after this shader.
  const float3 withBloom = 1.0 - (1.0 - bloom.rgb) * (1.0 - mapped);
  const float3 composed = pow(
    min(max(withBloom, 0.0), 65504.0), GameGammaExponent());
#if SPATCH_AGX_FULL_STRENGTH
  output.rgb = LimitDisplayPeak(composed);
#else
  output.rgb = lerp(
    composed, LimitDisplayPeak(composed), SPATCH_AGX_STRENGTH);
#endif
  output.a = 1.0;
}
)SPATCH_HLSL";

}  // namespace spatch::agx
