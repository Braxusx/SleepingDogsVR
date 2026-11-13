#pragma once

#include <Windows.h>
#include <dxgi.h>

#include <cstdint>
#include <cstdio>

#include <string>

#include "SmaaShaderSource.inl"

struct IDXGISwapChain;

namespace spatch::smaa {

struct Stats {
    unsigned long long present_count = 0;
    unsigned long long apply_count = 0;
    unsigned long long fail_count = 0;
    unsigned long long resize_count = 0;
    unsigned long long frame_limiter_paced = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    bool any_hook_retained = false;
    bool resources_ready = false;
    bool enabled = false;
};

// Stock AA may only be suppressed after the replacement has proven that it
// can render.  Keeping this as a pure policy helper makes the fail-open
// contract testable without constructing a D3D11 device in the unit tests.
[[nodiscard]] constexpr bool ShouldSuppressStockAa(bool enabled,
                                                   bool present_hook_installed,
                                                   bool resize_hook_installed,
                                                   bool resources_ready,
                                                   bool successful_pass) noexcept {
    return enabled && present_hook_installed && resize_hook_installed && resources_ready &&
           successful_pass;
}

[[nodiscard]] constexpr bool ShouldRunSmaaPresentPass(unsigned int flags) noexcept {
    // DXGI_PRESENT_TEST probes presentation status without displaying a frame;
    // touching the backbuffer or running three fullscreen passes is invalid.
    // DXGI_PRESENT_DO_NOT_WAIT is still a real Present. It must receive SMAA;
    // if DXGI rejects any real Present, the detour restores the source image
    // before returning so a later retry cannot filter the same frame twice.
    return (flags & DXGI_PRESENT_TEST) == 0;
}

[[nodiscard]] constexpr bool ShouldRestoreSmaaSource(unsigned int flags,
                                                     HRESULT present_result) noexcept {
    // A failed real Present does not consume the frame. Restore the saved
    // pre-SMAA image for every failure, not just the common nonblocking retry,
    // so an application retry cannot filter an already-filtered backbuffer.
    // TEST calls never run the SMAA pass and are excluded defensively.
    return (flags & DXGI_PRESENT_TEST) == 0 && present_result < 0;
}

// pass_completed includes rendering plus restoration of every captured game
// binding and retained COM reference. Publication additionally waits for the
// real Present result so failed/nonblocking presents cannot authorize stock-AA
// suppression on the next frame.
[[nodiscard]] constexpr bool ShouldPublishSmaaSuccess(bool pass_completed,
                                                      HRESULT present_result) noexcept {
    return pass_completed && present_result >= 0;
}

[[nodiscard]] constexpr bool ShouldInvalidateAfterSkippedPresent(
    unsigned int flags, HRESULT present_result) noexcept {
    // A successful TEST is only a status probe and preserves the last proven
    // frame. Every real skipped present, and every failed probe, fails open.
    return (flags & DXGI_PRESENT_TEST) == 0 || present_result < 0;
}

// Canonical SMAA performs perceptual edge detection from the stored sRGB
// values, but neighborhood blending in linear light. A typeless 8-bit resource
// permits both views without extra conversion passes.
[[nodiscard]] constexpr DXGI_FORMAT SmaaTypelessFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] constexpr DXGI_FORMAT SmaaLinearFormat(DXGI_FORMAT format) noexcept {
    switch (SmaaTypelessFormat(format)) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] constexpr DXGI_FORMAT SmaaSrgbFormat(DXGI_FORMAT format) noexcept {
    switch (SmaaTypelessFormat(format)) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

bool Initialize(std::uintptr_t module_base,
                std::uintptr_t device_slot,
                std::uintptr_t context_slot,
                std::uintptr_t swapchain_slot,
                std::uintptr_t present_rtv_slot);
// Opens DXGI hook discovery only after Hooks has committed its complete
// process-wide transaction. Initialize itself only prepares CPU-side state.
void Activate();
// Gates new SMAA work and releases D3D resources after active calls quiesce.
// The MinHook detours and their original trampolines are intentionally retained
// for process lifetime because this bundled fork has no safe disable-only API.
bool Shutdown();

void MaybeInstallHooks();
// Early install seam: called with a freshly created swap chain straight from
// the device-creation detour, before the game's first present. Installing at
// this moment captures the owned entry snapshot before the bundled Galaxy
// SDK's present patcher races us (its clobber between hook creation and the
// post-create capture left SMAA dead for whole sessions); anything it patches
// afterwards is repaired per frame from the owned snapshot.
void MaybeInstallHooksForSwapChain(IDXGISwapChain* known_swapchain);

// Edge-detection sensitivity override. 0 (or any out-of-range value) follows
// the canonical threshold of the selected preset; otherwise the override is
// compiled in as SMAA_THRESHOLD. Translucent pulsing world icons and mission
// markers flicker when their soft edges straddle the preset threshold, so a
// higher override (for example 0.15-0.25) removes that flicker.
[[nodiscard]] constexpr bool SmaaHasThresholdOverride(float threshold) noexcept {
    return threshold > 0.0f && threshold <= 0.5f;
}

// Compiled into the edge-detection shader after the preset block. The preset
// blocks define SMAA_THRESHOLD unconditionally, so the override must #undef
// first: a plain redefinition is a warning, and the mod compiles its shaders
// with warnings treated as errors (X3129).
[[nodiscard]] inline std::string SmaaThresholdDefine(float threshold) {
    char buffer[96]{};
    const int written =
        std::snprintf(buffer,
                      sizeof(buffer),
                      "#ifdef SMAA_THRESHOLD\n"
                      "#undef SMAA_THRESHOLD\n"
                      "#endif\n"
                      "#define SMAA_THRESHOLD %.6f\n",
                      static_cast<double>(threshold));
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
        return {};
    }
    return buffer;
}

// Assembles the exact shader source compiled at bootstrap.  Ordering is load
// bearing: the chunk containing the SMAA preset block defines SMAA_THRESHOLD
// unconditionally, so the threshold override MUST be appended after the
// chunks - its #undef then cleanly replaces the preset value.  An override
// placed before the chunks is a macro redefinition (a warning, and this
// module compiles shaders with warnings as errors) and would also be
// silently clobbered by the preset value.
[[nodiscard]] inline std::string AssembleSmaaShaderSource(
    std::string_view preset_define,
    bool threshold_override,
    float threshold) {
    static constexpr std::string_view kHeader = R"SMAAHLSL(
cbuffer SMAAConstants : register(b0) {
    float4 rtMetrics;
    float4 subsampleIndices;
};

#define SMAA_RT_METRICS rtMetrics
#define SMAA_HLSL_4_1 1
)SMAAHLSL";

    static constexpr std::string_view kFooter = R"SMAAHLSL(
Texture2D colorTex   : register(t0);
Texture2D edgesTex   : register(t4);
Texture2D blendTex   : register(t5);
Texture2D areaTex    : register(t6);
Texture2D searchTex  : register(t7);

float2 FullscreenUV(uint vertex_id) {
    return float2((vertex_id << 1) & 2, vertex_id & 2);
}

float4 FullscreenPosition(float2 uv) {
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

struct EdgeVsOut {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
    float4 offset[3] : TEXCOORD1;
};

struct BlendVsOut {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
    float2 pixcoord : TEXCOORD1;
    float4 offset[3] : TEXCOORD2;
};

struct NeighborhoodVsOut {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
    float4 offset : TEXCOORD1;
};

EdgeVsOut EdgeVS(uint vertex_id : SV_VertexID) {
    EdgeVsOut output;
    output.texcoord = FullscreenUV(vertex_id);
    output.position = FullscreenPosition(output.texcoord);
    SMAAEdgeDetectionVS(output.texcoord, output.offset);
    return output;
}

float2 EdgePS(EdgeVsOut input) : SV_TARGET {
    return SMAAColorEdgeDetectionPS(input.texcoord, input.offset, colorTex);
}

BlendVsOut BlendVS(uint vertex_id : SV_VertexID) {
    BlendVsOut output;
    output.texcoord = FullscreenUV(vertex_id);
    output.position = FullscreenPosition(output.texcoord);
    SMAABlendingWeightCalculationVS(output.texcoord, output.pixcoord, output.offset);
    return output;
}

float4 BlendPS(BlendVsOut input) : SV_TARGET {
    return SMAABlendingWeightCalculationPS(
        input.texcoord,
        input.pixcoord,
        input.offset,
        edgesTex,
        areaTex,
        searchTex,
        subsampleIndices);
}

NeighborhoodVsOut NeighborhoodVS(uint vertex_id : SV_VertexID) {
    NeighborhoodVsOut output;
    output.texcoord = FullscreenUV(vertex_id);
    output.position = FullscreenPosition(output.texcoord);
    SMAANeighborhoodBlendingVS(output.texcoord, output.offset);
    return output;
}

float4 NeighborhoodPS(NeighborhoodVsOut input) : SV_TARGET {
    return SMAANeighborhoodBlendingPS(input.texcoord, input.offset, colorTex, blendTex);
}
    )SMAAHLSL";

    std::string source;
    std::size_t shader_size =
        kHeader.size() + kFooter.size() + preset_define.size();
    for (const std::string_view chunk : kSmaaShaderSourceChunks) {
        shader_size += chunk.size();
    }
    source.reserve(shader_size + 128);
    source.append(kHeader);
    source.append(preset_define);
    for (const std::string_view chunk : kSmaaShaderSourceChunks) {
        source.append(chunk.data(), chunk.size());
    }
    if (threshold_override) {
        source.append(SmaaThresholdDefine(threshold));
    }
    source.append(kFooter);
    return source;
}

bool GetEnabled();
// True only when SMAA is enabled, both DXGI detours are owned, resources are
// live, and the current resource generation has completed a successful pass.
// Callers use this instead of GetEnabled() when deciding whether to skip the
// game's stock anti-aliasing path.
bool CanReplaceStockAa();
void SetEnabled(bool enabled);
bool GetDebugKeysEnabled();
void SetDebugKeysEnabled(bool enabled);
void SetPreset(int preset);

// R6 custom frame limiter. Paces real Present calls the RTSS way: each
// frame waits until previous + 1/fps before submitting, sleeping on a
// high-resolution waitable timer and spinning the last ~2 ms. Enabled with
// a positive fps; disabled (or fps 0) passes every call straight through.
// Call once from config bootstrap; safe to call again with new values.
void SetFrameLimiter(bool enabled, float fps);

// Read-only counters for the summary/debug surfaces.
unsigned long long FrameLimiterFramesPaced();
void SetEdgeThreshold(float threshold);
float GetEdgeThreshold();
// Non-FXAA fix for the world-icon flicker: a contrast-gated tent pass after
// the SMAA neighborhood blend that only smooths the high-frequency band
// (small pulsing world icons and mission markers) and leaves flat regions
// and SMAA's own blends untouched.
void SetIconStabilizer(bool enabled);
bool GetIconStabilizer();

Stats GetStats();

}  // namespace spatch::smaa
