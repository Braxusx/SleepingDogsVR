// Shadow filter constant scaling for Sleeping Dogs: Definitive Edition.
//
// The game's shadow shaders bake 2048-atlas texel constants directly into
// their bytecode, as confirmed in captured shader disassembly: the cascade visibility collector
// snaps its sampling to a 2048-texel grid (`uv*2048 + 0.5 -> round ->
// -frac/2048`) and the receivers use a PCF radius of 0.0008 (= 1.6/2048).
// Raising the shadow-map atlas to 4096 (ShenLong [Shadows] ShadowResolution)
// does not improve the image while those constants stay baked at 2048: the
// filters operate at half the map's texel density, which reads as low-res
// ped/Wei shadows.
//
// This component intercepts create_pipeline, matches the twelve shadow shaders
// by exact DXBC identity (CRC32 + size + the captured DXBC checksum), scales the baked constant families
// to the actual atlas size inside the SHEX/SHDR token stream, recomputes the
// DXBC checksum, and validates the patched bytecode with CreatePixelShader
// before installing it. A failed match or validation leaves the native
// shader completely untouched (same component-scoped guard contract).
//
// Three implementation facts verified by captured bytecode and WARP validation:
//  1. Only 4-byte-aligned words in the SHEX/SHDR body are patched. Byte-level
//     pattern replacement corrupts the token stream because the constants
//     also appear unaligned inside instruction tokens.
//  2. The D3D11 runtime validates the DXBC checksum at CreatePixelShader
//     (verified with a WARP device: any stale-checksum blob returns
//     E_INVALIDARG). The checksum is NOT stock MD5; it is the AMD GPUOpen
//     DXBCChecksum algorithm: custom-MD5 over file[0x14:] (magic + checksum
//     field excluded) with a custom finalization where the length word is
//     prepended to the last block and word[15] = (bit_count >> 2) | 1.
//  3. The shadow shaders are compiled at runtime by the game; identity
//     matching relies on deterministic recompilation (same source, flags and
//     settings), which is exactly what makes exact-identity matching reliable.
//
// Draw-time consumer census (CensusShadowConsumers debug key, default off):
// the component tracks CreatePixelShader bytecode identities, the bound
// pixel shader, and SRV bindings on BOTH the immediate context and the
// shared deferred-context vtable, so a real gameplay session reports which
// pixel shaders bind which scaled shadow maps and whether every shadow-pass
// viewport was corrected. Deferred contexts are a separate D3D11 class with
// their own vtable; hooking that vtable once covers every deferred instance.

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "Logger.h"
#include "ShadowScale.h"
#include "SmaaRuntime.h"
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

using spatch::shadow_scale::kScaledShadowMapGuid;

constexpr std::uint32_t kDefaultShadowResolution = 0;  // 0 = follow in-game setting

struct ShadowShaderId {
    std::uint32_t crc32;
    std::size_t size;
    std::array<std::uint8_t, 16> dxbc_checksum;
    // Per-shader filter-map override in texels (0 = use the global atlas_size
    // from ShadowResolution). A shader whose target map class scales to the
    // baked 2048 family keeps its native constants instead of being rescaled
    // for the 4096 cascade.
    std::uint32_t map_override = 0;
};

// Captured from the game's runtime-compiled shadow bytecode (2026-07-30 probe
// for the six; 2026-08-06 DumpShaders=1 census + disassembly for the rest).
// All 12 are verified shadow consumers: the 20 KB group are sun/visibility
// collectors (11x11 PCF kernel, texel grid snap), the 5-6 KB pair are
// deferred/CSM receivers (PCF radius), the remaining 0.0008 holders are
// additional PCF receivers. Each samples texShadow at t0 with the shadow
// atlas and bakes the texel size into the instruction stream. Exact
// shader 0x8507FE03 also contains the float 0.0008, but its reflected bindings
// and disassembly prove that it is an ambient-diffuse/depth pass rather than a
// shadow consumer, so it is deliberately excluded.
constexpr ShadowShaderId kShadowShaders[] = {
    // Sun/cascade collectors (11x11 PCF kernel + 2048-texel grid snap),
    // deferred/CSM receivers (0.0008 PCF radius) - all verified against the
    // runtime SRV census: each samples texShadow at t0
    // with the atlas and bakes the texel size into the instruction stream.
    {0xEE242C4F, 19596,
     {0x7C, 0x4C, 0x7E, 0xAD, 0xF9, 0xFF, 0x59, 0x78,
      0xEB, 0xEB, 0x8D, 0xB1, 0x78, 0x97, 0x74, 0xCA}},  // cascade visibility collector
    {0x0B309D0E, 19596,
     {0xA8, 0x93, 0xAA, 0x96, 0x2B, 0x61, 0xA9, 0x61,
      0x4D, 0x58, 0x32, 0x5F, 0x5F, 0xBC, 0xE2, 0x42}},  // collector variant
    {0x98F2BF47, 19940,
     {0x18, 0xD5, 0xFB, 0x52, 0x99, 0x0D, 0xCE, 0xAB,
      0x36, 0x8D, 0x0A, 0x2F, 0x29, 0x12, 0xD9, 0x38}},  // sun collector variant
    // 0x72D70119 is the sole sampler of the scaled 2048 character map
    // (1408 native -> 2048; the runtime census recorded 56K samples and no cascade
    // sampling). Its baked 2048-family constants are already correct for
    // a 2048 map, so map_override=2048 keeps them (identity patch).
    {0x72D70119, 20520,
     {0xAB, 0xE3, 0xB9, 0x7E, 0x13, 0x8C, 0x9B, 0x43,
      0xEF, 0x31, 0xF3, 0x76, 0x2B, 0x0E, 0x7C, 0x71}, 2048},  // character map
    {0xDD6C8356, 5448,
     {0x24, 0x2C, 0xB5, 0xDB, 0xFE, 0x75, 0x28, 0x0B,
      0xC8, 0x4B, 0x1D, 0x23, 0x15, 0xB3, 0xA6, 0x9E}},  // deferred spotlight receiver
    {0x5EBBA455, 6592,
     {0x3F, 0x6D, 0x72, 0x0A, 0x81, 0x35, 0x2D, 0xC2,
      0x77, 0x61, 0x8B, 0xB1, 0x6E, 0x26, 0xF1, 0x5C}},  // CSM sun receiver
    {0x056F4AC7, 11284,
     {0x10, 0x53, 0x94, 0x5A, 0xA2, 0x4C, 0x53, 0xC8,
      0x67, 0x44, 0x21, 0xE4, 0xF2, 0x36, 0x1D, 0x9F}},  // PCF receiver
    {0x193BFE44, 5624,
     {0xF7, 0x94, 0xB5, 0x6A, 0x65, 0x07, 0x2A, 0x24,
      0x94, 0x38, 0xA6, 0x25, 0xE7, 0xE8, 0xFB, 0x0C}},  // PCF receiver
    {0x223AA776, 12572,
     {0x7B, 0x47, 0xD7, 0x2B, 0x48, 0x13, 0x0D, 0xB3,
      0xE6, 0x17, 0x61, 0x9B, 0xDB, 0xA0, 0xD1, 0x76}},  // PCF receiver
    {0xB671C5AE, 11504,
     {0xB0, 0xD9, 0xD3, 0x7E, 0x42, 0xA2, 0x64, 0x61,
      0xE6, 0x62, 0xB5, 0x82, 0xF9, 0x52, 0x2A, 0x26}},  // PCF receiver
    {0xDCF9CD0C, 12400,
     {0xF1, 0x1B, 0x4D, 0x1E, 0xAD, 0xCA, 0xEA, 0x71,
      0x45, 0xCE, 0x4E, 0xD8, 0x07, 0xF1, 0x2B, 0x1B}},  // PCF receiver
    {0xE5E2CE1C, 6768,
     {0x6C, 0x0B, 0x40, 0x4C, 0x4F, 0xF0, 0x17, 0x34,
      0xC9, 0x4A, 0xDE, 0x40, 0xCF, 0xF6, 0x32, 0xA5}},  // PCF receiver
};
static_assert(std::size(kShadowShaders) == 12);

// float32 bit patterns baked for the 2048 atlas, verified against
// the fxc disassembly). The texel-size family is scaled by 2048/atlas, the
// grid-count family by atlas/2048.
struct ConstantFamily {
    std::uint32_t bits;
    bool is_grid_count;  // false = texel-size (reciprocal scale)
};

constexpr ConstantFamily kConstantFamilies[] = {
    {0x3A000000, false},  //  1/2048
    {0x3AC00000, false},  //  3/2048
    {0x3B200000, false},  //  5/2048
    {0x3A51B717, false},  //  0.0008 PCF radius (receivers)
    {0xBA000000, false},  // -1/2048 (texel-lock add)
    {0xBAC00000, false},  // -3/2048
    {0xBB200000, false},  // -5/2048
    {0x45000000, true},   //  2048.0 grid snap count
};

struct Settings {
    bool enabled = false;
    std::uint32_t atlas_size = 0;  // filter-scaling atlas in texels (0 = native filters)
    float texture_scale = 0.0f;    // shadow-map size multiplier (0 = no map scaling)
    bool dump_shaders = false;     // debug: write every pixel shader DXBC to shader_dump/
    std::wstring dump_directory;   // absolute dump dir when dump_shaders is set
    bool census_consumers = false; // debug: track which PS binds scaled shadow maps
};

// One scaled shadow map: the game's native size and the size we created.
struct ScaledTarget {
    std::uint32_t native_size = 0;
    std::uint32_t scaled_size = 0;
};

struct DeviceData {
    Settings settings;
    std::atomic<std::uint64_t> exact_identities_observed{0};
    std::atomic<std::uint64_t> patched_shaders{0};
    std::atomic<bool> logged_identity_failure{false};
    std::atomic<bool> logged_pipeline_exception{false};
    // Debug shader dump bookkeeping (dump_shaders mode only).
    std::mutex dump_mutex;
    std::set<std::pair<std::uint32_t, std::size_t>> dumped_shaders;
    std::atomic<std::uint64_t> dumped_shader_files{0};
    // Shadow-map texture scaling (CreateTexture2D / OMSetRenderTargets /
    // RSSetViewports detours). The {native, scaled} registry itself lives in
    // per-object D3D11 private data (kScaledShadowMapGuid), so there is no
    // map to maintain and no destroy tracking. Only capped diagnostics and
    // session summaries share this mutex.
    std::mutex summary_mutex;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> logged_texture_scales;
    std::atomic<std::uint64_t> scaled_textures_count{0};
    std::atomic<bool> logged_scaled_registry_failure{false};
    std::atomic<std::uint64_t> scaled_viewports{0};
    std::atomic<std::uint64_t> executed_command_lists{0};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> logged_dsv_binds;
    std::atomic<std::uint32_t> dsv_bind_logged{0};
    // Viewport corrections skipped because the map was simultaneously bound
    // as a pixel-shader SRV (a consumer pass sampling the map, not a shadow
    // render pass writing it).
    std::atomic<std::uint64_t> srv_blocked_corrections{0};
    // Capped PS-identity logging (census mode): the first few
    // corrections / consumer blocks log the bound pixel shader.
    std::atomic<std::uint32_t> correction_logged{0};
    std::atomic<std::uint32_t> srv_blocked_logged{0};
    std::atomic<bool> logged_already_scaled_viewport{false};
    // Draw-time consumer census (census_consumers debug mode only).
    // census_counts: key = crc<<32 | native<<16 | scaled, value = SRV bind
    // count (a pixel shader sampling that scaled shadow map). render_counts:
    // same key, value = draw count with that scaled map bound as DSV.
    std::mutex census_mutex;
    // Frame counter for the periodic census snapshot (NoteFrame cadence).
    std::atomic<std::uint32_t> present_frames{0};
    std::unordered_map<ID3D11PixelShader*, std::uint32_t> ps_crc;
    std::unordered_map<std::uint64_t, std::uint64_t> census_counts;
    std::unordered_map<std::uint64_t, std::uint64_t> render_counts;
    // Per-class viewport correction counts, reported at device destroy.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> viewport_corrections;
};

// SPatch.asi serves exactly one game device: a single static data holder
// replaces the per-device ReShade private data. g_device_data points here
// only while the native hooks are armed and accepting calls.
DeviceData g_data;
DeviceData* g_device_data = nullptr;

HMODULE g_module = nullptr;

enum LogLevel { kLogInfo, kLogWarning, kLogError };

void Log(LogLevel level, const char* format, ...) noexcept {
    std::array<char, 1024> message{};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message.data(), message.size(), _TRUNCATE, format, args);
    va_end(args);
    switch (level) {
        case kLogWarning:
            spatch::log::Warn(message.data());
            break;
        case kLogError:
            spatch::log::Error(message.data());
            break;
        default:
            spatch::log::Info(message.data());
            break;
    }
}

// ---------------------------------------------------------------------------
// CRC-32 (standard reflected variant, polynomial 0xEDB88320, init 0xFFFFFFFF,
// final inversion). Byte-identical to the ReShade crc32_hash.hpp variant that
// produced the identity CRCs in kShadowShaders, so every captured identity
// still matches.
// ---------------------------------------------------------------------------

constexpr std::array<std::uint32_t, 256> MakeCrc32Table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < 256; ++index) {
        std::uint32_t crc = index;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0u ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
        table[index] = crc;
    }
    return table;
}

constexpr auto kCrc32Table = MakeCrc32Table();

std::uint32_t ComputeCrc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (; size != 0; --size, ++data) {
        crc = (crc >> 8) ^ kCrc32Table[(crc ^ (*data)) & 0xFFu];
    }
    return ~crc;
}

std::wstring ModuleDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            g_module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        if (path.size() >= 32768) {
            return {};
        }
        path.resize((std::min)(path.size() * 2, std::size_t{32768}), L'\0');
    }
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return {};
    }
    path.resize(separator + 1);
    return path;
}

// ---------------------------------------------------------------------------
// Minimal strict INI reader for SPatch.ini. SPatch's Config owns the module's
// canonical settings; this reader only covers the ShadowScale keys so the
// component stays self-contained and no Config headers are pulled in.
// Keys and values in scope are ASCII; sections and keys match
// case-insensitively; ';' and '#' line comments are skipped.
// ---------------------------------------------------------------------------

std::string ReadIniFileBytes(const std::wstring& path) noexcept {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) {
        return {};
    }
    std::string content;
    char buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        content.append(buffer, read);
    }
    std::fclose(file);
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);  // skip a UTF-8 byte-order mark
    }
    return content;
}

std::string_view TrimAscii(std::string_view text) noexcept {
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
               c == '\f' || c == '\v';
    };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

char LowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (LowerAscii(a[index]) != LowerAscii(b[index])) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> FindIniValue(
    const std::string& content,
    std::string_view section,
    std::initializer_list<std::string_view> keys) noexcept {
    std::string_view rest = content;
    std::string current_section;
    while (!rest.empty()) {
        const std::size_t newline = rest.find('\n');
        const std::string_view line = TrimAscii(
            rest.substr(0, newline == std::string_view::npos ? rest.size()
                                                             : newline));
        rest = newline == std::string_view::npos
                   ? std::string_view{}
                   : rest.substr(newline + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }
        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            if (close != std::string_view::npos) {
                current_section = std::string(TrimAscii(line.substr(1, close - 1)));
            }
            continue;
        }
        if (!EqualsIgnoreCase(current_section, section)) {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = TrimAscii(line.substr(0, equals));
        const std::string_view value = TrimAscii(line.substr(equals + 1));
        for (const std::string_view candidate : keys) {
            if (EqualsIgnoreCase(key, candidate)) {
                return std::string(value);
            }
        }
    }
    return std::nullopt;
}

int ParseIniInt(const std::optional<std::string>& value, int fallback) noexcept {
    if (!value || value->empty()) {
        return fallback;
    }
    std::size_t index = 0;
    if ((*value)[index] == '+' || (*value)[index] == '-') {
        ++index;
    }
    if (index >= value->size()) {
        return fallback;
    }
    for (std::size_t position = index; position < value->size(); ++position) {
        if ((*value)[position] < '0' || (*value)[position] > '9') {
            return fallback;  // strict: any non-digit rejects the value
        }
    }
    return std::atoi(value->c_str());
}

bool ParseIniBool(const std::optional<std::string>& value, bool fallback) noexcept {
    if (!value || value->empty()) {
        return fallback;
    }
    if (EqualsIgnoreCase(*value, "1") || EqualsIgnoreCase(*value, "true") ||
        EqualsIgnoreCase(*value, "yes") || EqualsIgnoreCase(*value, "on")) {
        return true;
    }
    return false;
}

Settings LoadSettings() {
    Settings settings;
    const std::wstring directory = ModuleDirectory();
    if (directory.empty()) {
        Log(kLogError,
            "[SPatch-ShadowScale] Could not resolve the SPatch module directory; retaining native shadow filters.");
        return settings;
    }
    const std::wstring path = directory + L"SPatch.ini";
    const std::string content = ReadIniFileBytes(path);
    if (content.empty()) {
        Log(kLogInfo,
            "[SPatch-ShadowScale] SPatch.ini not readable; retaining native shadow filters.");
        return settings;
    }

    const int shadow_resolution = std::clamp(
        ParseIniInt(FindIniValue(content, "Shadows",
                                 {"ShadowResolution", "shadow_resolution"}),
                    static_cast<int>(kDefaultShadowResolution)),
        0, 4096);
    // Clamp: anything below 2048 keeps native maps, 2048 doubles the map
    // density floor, anything above reaches the engine's own 4096 tier.
    const int clamped_resolution = shadow_resolution < 2048
        ? 0
        : (shadow_resolution == 2048 ? 2048 : 4096);
    const bool dump_shaders = ParseIniBool(
        FindIniValue(content, "Debug", {"DumpShaders", "dump_shaders"}), false);
    const bool census_consumers = ParseIniBool(
        FindIniValue(content, "Debug",
                     {"CensusShadowConsumers", "census_shadow_consumers"}),
        false);

    // ShadowResolution is the single owner of both map and filter density.
    // Keeping them coupled prevents an INI-only filter override from applying
    // 4096 constants to native/2048 maps. A nonzero ShadowResolution maps the
    // actual captured D3D resource classes to fixed targets (an idempotent
    // target mapping, never multiplicative).
    float texture_scale = 0.0f;
    if (clamped_resolution >= 2048) {
        texture_scale = static_cast<float>(clamped_resolution) / 2048.0f;
    }
    const std::uint32_t atlas_size = clamped_resolution >= 2048
        ? static_cast<std::uint32_t>(clamped_resolution)
        : 0u;
    if (texture_scale <= 0.0f && atlas_size == 0) {
        Log(kLogInfo,
            "[SPatch-ShadowScale] ShadowResolution=%d -> native shadow maps and filters retained.",
            clamped_resolution);
        return settings;
    }
    settings.enabled = true;
    settings.dump_shaders = dump_shaders;
    settings.census_consumers = census_consumers;
    settings.texture_scale = texture_scale;
    settings.atlas_size = atlas_size;
    if (dump_shaders) {
        settings.dump_directory = directory + L"ShaderDump\\";
        CreateDirectoryW(settings.dump_directory.c_str(), nullptr);
    }
    Log(kLogInfo,
        "[SPatch-ShadowScale] ShadowResolution=%d texture_scale=%.3f filter_atlas=%u shaders=%zu census=%d.",
        clamped_resolution, settings.texture_scale,
        settings.atlas_size, std::size(kShadowShaders), census_consumers ? 1 : 0);
    return settings;
}

// ---------------------------------------------------------------------------
// DXBC checksum (AMD GPUOpen DXBCChecksum.cpp, verified against fxc output
// and D3D11 runtime validation).
// ---------------------------------------------------------------------------

namespace checksum {

inline std::uint32_t RotateLeft(std::uint32_t value, std::uint32_t count) {
    return (value << count) | (value >> (32 - count));
}

struct Md5Context {
    std::uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    std::uint8_t buffer[64]{};
    std::size_t buffered = 0;
};

constexpr std::uint32_t kShift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

constexpr std::uint32_t kConstant[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

void Transform(std::uint32_t state[4], const std::uint32_t words[16]) {
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (std::uint32_t i = 0; i < 64; ++i) {
        std::uint32_t f = 0;
        std::uint32_t g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const std::uint32_t t = RotateLeft(a + f + kConstant[i] + words[g], kShift[i]);
        a = d;
        d = c;
        c = b;
        b = b + t;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void Update(Md5Context& ctx, const std::uint8_t* data, std::size_t size) {
    while (size > 0) {
        const std::size_t take =
            (std::min)(size, 64 - ctx.buffered);
        std::memcpy(ctx.buffer + ctx.buffered, data, take);
        ctx.buffered += take;
        data += take;
        size -= take;
        if (ctx.buffered == 64) {
            std::uint32_t words[16];
            std::memcpy(words, ctx.buffer, 64);
            Transform(ctx.state, words);
            ctx.buffered = 0;
        }
    }
}

constexpr std::uint8_t kPadding[64] = {
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Finalize with the DXBCChecksum variant. `last` is the final partial chunk
// (0..63 bytes) that could not fill a full 64-byte block; `bit_count` is
// (hashed_size * 8). This mirrors the validated reference exactly:
//   - last >= 56: complete the block with standard padding, then one extra
//     transform carries the length in word 0 and (bits>>2)|1 in word 15.
//   - last <  56: the length word is prepended to the final block, then the
//     last data bytes, then padding, with word 15 overwritten by
//     (bits>>2)|1 and a single transform.
void Finalize(Md5Context& ctx,
              const std::uint8_t* last,
              std::size_t last_size,
              std::uint32_t bit_count,
              std::uint8_t checksum[16]) {
    std::uint32_t words[16] = {};
    if (last_size >= 56) {
        Update(ctx, last, last_size);
        Update(ctx, kPadding, 64 - last_size);
        words[0] = bit_count;
        words[15] = (bit_count >> 2) | 1;
        Transform(ctx.state, words);
    } else {
        const std::uint32_t bit_count_le = bit_count;
        Update(ctx, reinterpret_cast<const std::uint8_t*>(&bit_count_le), 4);
        if (last_size > 0) {
            Update(ctx, last, last_size);
        }
        std::memcpy(ctx.buffer + ctx.buffered, kPadding, 64 - ctx.buffered);
        std::memcpy(words, ctx.buffer, 64);
        words[15] = (bit_count >> 2) | 1;
        Transform(ctx.state, words);
    }
    std::memcpy(checksum, ctx.state, 16);
}

void Compute(const std::uint8_t* data, std::size_t size, std::uint8_t checksum[16]) {
    // Hash the container excluding the 4-byte magic and the 16-byte checksum.
    if (size <= 0x14) {
        std::memset(checksum, 0, 16);
        return;
    }
    const std::size_t body_size = size - 0x14;
    const std::uint8_t* body = data + 0x14;
    Md5Context ctx;
    const std::size_t full = body_size & ~std::size_t{0x3F};
    Update(ctx, body, full);
    Finalize(ctx, body + full, body_size - full,
             static_cast<std::uint32_t>(body_size * 8), checksum);
}

}  // namespace checksum

// ---------------------------------------------------------------------------
// Constant patching
// ---------------------------------------------------------------------------

std::uint32_t ScaleBits(std::uint32_t bits, float scale) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    value *= scale;
    std::uint32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

// Patches aligned SHEX/SHDR words. Returns false when the blob is not a
// well-formed DXBC with a patchable body (or when nothing needed patching).
bool PatchShadowShaderConstants(const std::uint8_t* code,
                                std::size_t size,
                                std::uint32_t atlas_size,
                                std::vector<std::uint8_t>& out,
                                std::size_t* changed_count) {
    if (changed_count) {
        *changed_count = 0;
    }
    if (!code || size < 64 || std::memcmp(code, "DXBC", 4) != 0) {
        return false;
    }
    std::uint32_t chunk_count = 0;
    std::memcpy(&chunk_count, code + 0x1C, 4);
    std::size_t body_start = 0;
    std::size_t body_size = 0;
    for (std::uint32_t index = 0; index < chunk_count; ++index) {
        const std::size_t entry = 0x20 + static_cast<std::size_t>(index) * 4;
        if (entry + 4 > size) {
            return false;
        }
        std::uint32_t chunk_offset = 0;
        std::memcpy(&chunk_offset, code + entry, 4);
        if (chunk_offset + 8 > size) {
            return false;
        }
        const char* fourcc = reinterpret_cast<const char*>(code + chunk_offset);
        if (std::memcmp(fourcc, "SHEX", 4) != 0 &&
            std::memcmp(fourcc, "SHDR", 4) != 0) {
            continue;
        }
        std::uint32_t chunk_size = 0;
        std::memcpy(&chunk_size, code + chunk_offset + 4, 4);
        const std::size_t start = static_cast<std::size_t>(chunk_offset) + 8;
        if (start + chunk_size > size || (chunk_size & 3) != 0) {
            return false;
        }
        body_start = start;
        body_size = chunk_size;
        break;
    }
    if (body_size == 0) {
        return false;
    }

    if (atlas_size <= 2048) {
        return false;  // native 2048 density is already correct
    }
    const float texel_scale = 2048.0f / static_cast<float>(atlas_size);
    const float grid_scale = static_cast<float>(atlas_size) / 2048.0f;
    const ConstantFamily* const families = kConstantFamilies;
    const std::size_t family_count = std::size(kConstantFamilies);

    out.assign(code, code + size);
    std::size_t changed = 0;
    for (std::size_t family_index = 0; family_index < family_count; ++family_index) {
        const ConstantFamily& family = families[family_index];
        const float scale = family.is_grid_count ? grid_scale : texel_scale;
        const std::uint32_t replacement = ScaleBits(family.bits, scale);
        if (replacement == family.bits) {
            continue;
        }
        const std::uint32_t pattern = family.bits;
        for (std::size_t offset = body_start;
             offset + 4 <= body_start + body_size;
             offset += 4) {
            std::uint32_t word = 0;
            std::memcpy(&word, out.data() + offset, 4);
            if (word == pattern) {
                std::memcpy(out.data() + offset, &replacement, 4);
                ++changed;
            }
        }
    }
    if (changed == 0) {
        return false;
    }
    if (changed_count) {
        *changed_count = changed;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Native shadow-map scaling (MinHook detours)
// ---------------------------------------------------------------------------
//
// The game creates every shadow map as a standalone square R24G8_TYPELESS
// (format 44) depth target with DSV|SRV binding from one call site
// (sdhdship.exe+0xA19929) and renders each one with a full-texture viewport
// (the capture showed vp=WxW matching dsv=WxW for every shadow pass). The SPatch ASI
// patch that resized these maps targets a setup function that is dead code at
// runtime, so the maps never changed. Instead of patching game code, these
// detours scale the maps at CreateTexture2D and scale the shadow-pass
// viewport to match (via OMSetRenderTargets DSV tracking plus a draw-time
// correction for the game's viewport-before-DSV ordering), so the projection
// stays exact and the filter constants scaled in the CreatePixelShader detour match the
// map. Any install failure disables map scaling AND filter scaling together,
// so the game is never left with mismatched maps and filters. Caveat: the
// context detours are installed on both the immediate and shared deferred
// context vtables. The game actively records deferred command lists, so either
// hook set failing disables map and filter scaling together.

constexpr DXGI_FORMAT kR24G8TypelessFormat = DXGI_FORMAT_R24G8_TYPELESS;

ID3D11Device* g_native_device = nullptr;
void* g_original_create_texture2d = nullptr;
void* g_original_create_pixel_shader = nullptr;
void* g_hook_target_create_texture2d = nullptr;
void* g_hook_target_create_pixel_shader = nullptr;
std::atomic<bool> g_native_module_pinned = false;
std::atomic<bool> g_native_hook_accepting = false;
std::atomic<std::uint32_t> g_native_hook_accepted_calls = 0;
std::atomic<std::uint64_t> g_context_state_epoch = 1;

// Context vtable hook sets. The immediate context and deferred contexts are
// different D3D11 classes with separate vtables; every deferred context
// shares one vtable, so one hook set covers all of them. Detours route to
// the correct set from the context type. This is what makes shadow passes
// recorded on deferred contexts get viewport scaling and census coverage too.
struct ContextHookSet {
    void* om_set_render_targets = nullptr;
    void* om_set_render_targets_and_uavs = nullptr;
    void* rs_set_viewports = nullptr;
    void* ps_set_shader = nullptr;
    void* ps_set_shader_resources = nullptr;
    void* draw = nullptr;
    void* draw_indexed = nullptr;
    void* draw_instanced = nullptr;
    void* draw_indexed_instanced = nullptr;
    void* draw_indexed_instanced_indirect = nullptr;
    void* draw_instanced_indirect = nullptr;
    void* draw_auto = nullptr;
    void* execute_command_list = nullptr;
    void* clear_state = nullptr;
    void* finish_command_list = nullptr;
};
ContextHookSet g_immediate_hooks;
ContextHookSet g_deferred_hooks;
void* g_immediate_vtable = nullptr;
void* g_deferred_vtable = nullptr;

// The pinned MinHook fork enables each target inside MH_CreateHook. While the
// immediate set is being installed, a shared target may therefore receive a
// deferred-context call before InstallContextHooks copies that trampoline into
// the deferred set. The target's directly published trampoline is already in
// the alternate set at that point, so select per method and fall back there.
void* OriginalFor(ID3D11DeviceContext* context,
                  void* ContextHookSet::* member) noexcept {
    ContextHookSet& preferred =
        context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED
            ? g_deferred_hooks
            : g_immediate_hooks;
    ContextHookSet& alternate = &preferred == &g_deferred_hooks
        ? g_immediate_hooks
        : g_deferred_hooks;
    void* const original = preferred.*member;
    return original != nullptr ? original : alternate.*member;
}

// The shadow DSV bound by the most recent OMSetRenderTargets on one context.
// Shadow passes bind their DSV, then set a full-texture viewport, so the
// RSSetViewports detour scales the viewport for the bound scaled map.
struct CurrentShadowDsv {
    ID3D11Resource* resource = nullptr;
    std::uint32_t native_size = 0;
    std::uint32_t scaled_size = 0;
};

// Snapshot of the most recent RSSetViewports call on one context. The game
// binds shadow DSVs and viewports in either order; the draw-time correction
// (EnsureShadowViewportScaled) re-applies the viewport at the scaled size
// right before the draw when the bound shadow DSV is scaled but the viewport
// is still at the native size.
struct CurrentViewports {
    UINT count = 0;
    D3D11_VIEWPORT values[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
};

struct ContextState {
    std::uint64_t epoch = 0;
    ID3D11PixelShader* current_ps = nullptr;
    std::array<ID3D11Resource*, 32> ps_srvs{};
    CurrentShadowDsv shadow_dsv;
    CurrentViewports viewports;
};

// D3D11 contexts may be interleaved on one worker thread. Keying the tracker
// by context prevents an immediate-context bind from contaminating a deferred
// recording (and vice versa). The D3D11 threading contract serializes calls on
// an individual context, so no cross-thread synchronization is needed here.
thread_local std::unordered_map<ID3D11DeviceContext*, ContextState>
    g_context_states;
thread_local ContextState g_overflow_context_state;

ContextState& OverflowState(std::uint64_t epoch) noexcept {
    // Never let two untracked contexts inherit one another's DSV or viewport
    // state. Reset the thread-local overflow slot on every fallback access.
    g_overflow_context_state = ContextState{};
    g_overflow_context_state.epoch = epoch;
    return g_overflow_context_state;
}

ContextState& StateFor(ID3D11DeviceContext* context) noexcept {
    const std::uint64_t epoch =
        g_context_state_epoch.load(std::memory_order_acquire);
    auto found = g_context_states.find(context);
    if (found == g_context_states.end()) {
        constexpr std::size_t kMaxTrackedContextsPerThread = 64;
        if (g_context_states.size() >= kMaxTrackedContextsPerThread) {
            // Fail closed for pathological context churn: an overflow context
            // never inherits another context's DSV/viewport state.
            return OverflowState(epoch);
        }
        try {
            found = g_context_states.emplace(context, ContextState{}).first;
        } catch (...) {
            // State tracking is optional. Allocation pressure must not escape
            // a D3D detour or prevent the native call from being forwarded.
            return OverflowState(epoch);
        }
    }
    ContextState& state = found->second;
    if (state.epoch != epoch) {
        state = ContextState{};
        state.epoch = epoch;
    }
    return state;
}

class NativeHookCallGuard {
public:
    NativeHookCallGuard() noexcept {
        if (!g_native_hook_accepting.load(std::memory_order_acquire)) {
            return;
        }
        g_native_hook_accepted_calls.fetch_add(1, std::memory_order_acq_rel);
        if (!g_native_hook_accepting.load(std::memory_order_acquire)) {
            g_native_hook_accepted_calls.fetch_sub(1, std::memory_order_release);
            return;
        }
        accepted_ = true;
    }

    NativeHookCallGuard(const NativeHookCallGuard&) = delete;
    NativeHookCallGuard& operator=(const NativeHookCallGuard&) = delete;

    ~NativeHookCallGuard() {
        if (accepted_) {
            g_native_hook_accepted_calls.fetch_sub(1, std::memory_order_release);
        }
    }

    bool accepted() const noexcept { return accepted_; }

private:
    bool accepted_ = false;
};

std::uint32_t ScaleShadowMapSize(std::uint32_t native) noexcept {
    const float scale = g_device_data ? g_device_data->settings.texture_scale : 0.0f;
    if (scale <= 0.0f) {
        return native;
    }
    // ShadowResolution >= 2048 floors every shadow map at 2048 (512, 1024
    // and 1408 all floor to 2048 - including the 512 class, which could
    // otherwise hold character shadows at 1024 density); 4096 doubles the
    // full-size 2048 targets to 4096. The engine's own 4096 targets are
    // never exceeded. Native sizes: 512x512 x6, 1024x1024 x9, 1408x1408 x3,
    // 2048x2048 x6 in the captured allocation census.
    if (native < 2048) {
        return 2048u;  // 512, 1024, 1408 all floor to 2048
    }
    if (native >= 4096) {
        return 4096u;  // never exceed the engine's own top-tier targets
    }
    return static_cast<std::uint32_t>(static_cast<float>(native) * scale);  // 2048 -> 2048/4096
}

bool IsCapturedShadowMapSize(std::uint32_t size) noexcept {
    // These are the only square depth-map classes observed at the verified
    // shadow allocation site. Do not resize unrelated R24G8 depth targets
    // merely because they share the same format and bind flags.
    return size == 512u || size == 1024u || size == 1408u ||
           size == 2048u || size == 4096u;
}

// Reads the {native, scaled} stamp a scaled shadow map carries in its own
// D3D11 private data. Replaces the former mutex-protected unordered_map: the
// stamp dies with the object, so there are no stale entries and no destroy
// tracking. Allocation-free and lock-free on every call site.
bool LookupScaledTarget(ID3D11Resource* resource,
                        std::uint32_t& native_size,
                        std::uint32_t& scaled_size) noexcept {
    native_size = 0;
    scaled_size = 0;
    if (!resource) {
        return false;
    }
    ScaledTarget stored{};
    UINT size = sizeof(stored);
    if (FAILED(resource->GetPrivateData(kScaledShadowMapGuid, &size, &stored)) ||
        size != sizeof(stored)) {
        return false;
    }
    native_size = stored.native_size;
    scaled_size = stored.scaled_size;
    return true;
}

void TrackShadowDsv(ID3D11DeviceContext* context,
                    ID3D11DepthStencilView* dsv, bool pure_depth,
                    std::uint32_t rtv_count, std::uint32_t uav_count) noexcept {
    CurrentShadowDsv& current = StateFor(context).shadow_dsv;
    current = {};
    if (!dsv || !g_device_data) {
        return;
    }
    try {
        // Depth-only gate: only a pass with no real color output can be
        // rendering into the shadow map. The engine binds the map as DSV
        // together with an all-null render-target array (the classic D3D11
        // "no color output" idiom), so num_views == 0 is the wrong test. A
        // real color target makes this a consumer pass and stays unscaled.
        ComPtr<ID3D11Resource> resource;
        if (pure_depth || g_device_data->settings.census_consumers) {
            dsv->GetResource(resource.ReleaseAndGetAddressOf());
        }
        if (!pure_depth) {
            if (g_device_data->settings.census_consumers && resource) {
                std::uint32_t ignored_native = 0;
                std::uint32_t ignored_scaled = 0;
                const bool scaled = LookupScaledTarget(
                    resource.Get(), ignored_native, ignored_scaled);
                const std::uint32_t bind_log =
                    g_device_data->dsv_bind_logged.fetch_add(
                        1, std::memory_order_relaxed);
                if (bind_log < 8 && scaled) {
                    Log(kLogWarning,
                        "[SPatch-ShadowScale] Scaled shadow map skipped: bound as DSV with %u RTV(s)/%u UAV(s) - not a depth-only shadow pass.",
                        static_cast<unsigned>(rtv_count),
                        static_cast<unsigned>(uav_count));
                }
            }
            return;
        }
        if (!resource) {
            return;
        }
        bool tracked = false;
        std::uint32_t native_size = 0;
        std::uint32_t scaled_size = 0;
        if (LookupScaledTarget(resource.Get(), native_size, scaled_size)) {
            const std::uint32_t bind_log =
                g_device_data->dsv_bind_logged.fetch_add(
                    1, std::memory_order_relaxed);
            if (bind_log < 8 && g_device_data->settings.census_consumers) {
                Log(kLogInfo,
                    "[SPatch-ShadowScale] Scaled shadow DSV bind #%u: map %ux%u depth-only (%u RTVs, %u UAVs).",
                    bind_log + 1,
                    static_cast<unsigned>(native_size),
                    static_cast<unsigned>(scaled_size),
                    static_cast<unsigned>(rtv_count),
                    static_cast<unsigned>(uav_count));
            }
            current.resource = resource.Get();
            current.native_size = native_size;
            current.scaled_size = scaled_size;
            const auto pair = std::make_pair(native_size, scaled_size);
            bool known = false;
            {
                std::lock_guard<std::mutex> lock(g_device_data->summary_mutex);
                for (const auto& existing : g_device_data->logged_dsv_binds) {
                    if (existing == pair) {
                        known = true;
                        break;
                    }
                }
                if (!known && g_device_data->logged_dsv_binds.size() < 8) {
                    try {
                        g_device_data->logged_dsv_binds.push_back(pair);
                        tracked = true;
                    } catch (...) {
                        // Capped bind logging is diagnostic only. Keep the
                        // functional DSV tracker populated.
                    }
                }
            }
        }
        if (tracked) {
            Log(kLogInfo,
                "[SPatch-ShadowScale] Shadow DSV bound: map %ux%u tracked for viewport scaling.",
                static_cast<unsigned>(current.native_size),
                static_cast<unsigned>(current.scaled_size));
        }
    } catch (...) {
        // DSV tracking and its capped diagnostics are optional. Preserve the
        // native bind if a bookkeeping container cannot allocate.
        current = {};
    }
}

// Counts one viewport correction for a (native, scaled) map class; the
// per-class totals are reported in the device-destroy session summary.
void CountViewportCorrection(DeviceData* data,
                             std::uint32_t native,
                             std::uint32_t scaled) noexcept {
    try {
        std::lock_guard<std::mutex> lock(data->summary_mutex);
        ++data->viewport_corrections[std::make_pair(native, scaled)];
    } catch (...) {
        // Session-summary diagnostics must never interrupt a D3D draw.
    }
}

// Census: records one draw into a scaled shadow map (the map bound as DSV)
// against the current pixel shader's identity. Diagnostic only.
void RecordShadowRenderIfCensus(ID3D11DeviceContext* context) noexcept {
    DeviceData* const data = g_device_data;
    const ContextState& state = StateFor(context);
    if (!data || !data->settings.census_consumers || !state.shadow_dsv.resource) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(data->census_mutex);
        std::uint32_t crc = 0;
        const auto it = data->ps_crc.find(state.current_ps);
        if (it != data->ps_crc.end()) {
            crc = it->second;
        }
        const std::uint64_t key =
            (static_cast<std::uint64_t>(crc) << 32) |
            (static_cast<std::uint64_t>(state.shadow_dsv.native_size) << 16) |
            state.shadow_dsv.scaled_size;
        if (data->render_counts.size() < 512 ||
            data->render_counts.count(key) != 0) {
            ++data->render_counts[key];
        }
    } catch (...) {
        // Census data is diagnostic only.
    }
}

using CreateTexture2DFunction = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
    const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);

HRESULT RetryNativeTextureCreation(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC* native_desc,
    const D3D11_SUBRESOURCE_DATA* initial_data,
    ID3D11Texture2D** out_texture) noexcept {
    if (out_texture != nullptr && *out_texture != nullptr) {
        (*out_texture)->Release();
        *out_texture = nullptr;
    }
    return reinterpret_cast<CreateTexture2DFunction>(
        g_original_create_texture2d)(
            device, native_desc, initial_data, out_texture);
}

// Stamps the {native, scaled} sizes onto the created texture's own private
// data. Replaces the former mutex-protected unordered_map registry. Returns
// false when the stamp fails: tracking is mandatory for an enlarged texture
// (it drives both DSV recognition and viewport correction), so the caller
// retries with the native descriptor (fail-closed).
bool StampScaledTarget(DeviceData* data,
                       ID3D11Resource* resource,
                       std::uint32_t native_size,
                       std::uint32_t scaled_size,
                       bool& reported) noexcept {
    reported = false;
    const ScaledTarget stored{native_size, scaled_size};
    const HRESULT stamp_result = resource->SetPrivateData(
        kScaledShadowMapGuid, sizeof(stored), &stored);
    if (FAILED(stamp_result)) {
        return false;
    }
    try {
        const auto pair = std::make_pair(native_size, scaled_size);
        std::lock_guard<std::mutex> lock(data->summary_mutex);
        bool known = false;
        for (const auto& existing : data->logged_texture_scales) {
            if (existing == pair) {
                known = true;
                break;
            }
        }
        if (!known && data->logged_texture_scales.size() < 8) {
            try {
                data->logged_texture_scales.push_back(pair);
                reported = true;
            } catch (...) {
                // This capped message census is diagnostic only. The
                // mandatory private-data stamp remains valid.
            }
        }
        return true;
    } catch (...) {
        // Only the capped reporting failed; the stamp itself is in place.
        return true;
    }
}

HRESULT STDMETHODCALLTYPE DetourCreateTexture2D(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* initial_data,
    ID3D11Texture2D** out_texture) {
    NativeHookCallGuard hook_call;
    if (!hook_call.accepted() || !desc) {
        return reinterpret_cast<CreateTexture2DFunction>(
            g_original_create_texture2d)(device, desc, initial_data, out_texture);
    }
    D3D11_TEXTURE2D_DESC scaled_desc = *desc;
    bool scale_this = false;
    constexpr UINT kRequiredShadowBindings =
        D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (g_device_data && g_device_data->settings.texture_scale > 0.0f &&
        initial_data == nullptr &&
        scaled_desc.Format == kR24G8TypelessFormat &&
        scaled_desc.Width == scaled_desc.Height &&
        IsCapturedShadowMapSize(scaled_desc.Width) && scaled_desc.ArraySize == 1 &&
        scaled_desc.MipLevels == 1 && scaled_desc.SampleDesc.Count == 1 &&
        scaled_desc.Usage == D3D11_USAGE_DEFAULT &&
        (scaled_desc.BindFlags & kRequiredShadowBindings) ==
            kRequiredShadowBindings) {
        const std::uint32_t scaled = ScaleShadowMapSize(scaled_desc.Width);
        if (scaled != scaled_desc.Width) {
            scaled_desc.Width = scaled;
            scaled_desc.Height = scaled;
            scale_this = true;
        }
    }
    const HRESULT result = reinterpret_cast<CreateTexture2DFunction>(
        g_original_create_texture2d)(device, &scaled_desc, initial_data, out_texture);
    if (SUCCEEDED(result) && out_texture && *out_texture) {
        DeviceData* const data = g_device_data;
        if (data) {
            ComPtr<ID3D11Resource> resource;
            (*out_texture)->QueryInterface(__uuidof(ID3D11Resource),
                reinterpret_cast<void**>(resource.ReleaseAndGetAddressOf()));
            if (resource) {
                if (scale_this) {
                    bool reported = false;
                    if (!StampScaledTarget(
                            data, resource.Get(), desc->Width,
                            scaled_desc.Width, reported)) {
                        if (!data->logged_scaled_registry_failure.exchange(
                                true, std::memory_order_relaxed)) {
                            Log(kLogWarning,
                                "[SPatch-ShadowScale] Scaled shadow-map private-data stamp failed; retrying the native descriptor.");
                        }
                        return RetryNativeTextureCreation(
                            device, desc, initial_data, out_texture);
                    }
                    if (reported) {
                        Log(kLogInfo,
                            "[SPatch-ShadowScale] Scaled shadow map %ux%u -> %ux%u.",
                            static_cast<unsigned>(desc->Width), static_cast<unsigned>(desc->Height),
                            static_cast<unsigned>(scaled_desc.Width), static_cast<unsigned>(scaled_desc.Height));
                    }
                    data->scaled_textures_count.fetch_add(1, std::memory_order_relaxed);
                }
                // Non-shadow textures need no bookkeeping: the private-data
                // stamp lives and dies with each scaled object, so there is
                // no stale-entry cleanup.
            }
            if (scale_this && !resource) {
                if (!data->logged_scaled_registry_failure.exchange(
                        true, std::memory_order_relaxed)) {
                    Log(kLogWarning,
                        "[SPatch-ShadowScale] Scaled shadow-map resource identity was unavailable; retrying the native descriptor.");
                }
                return RetryNativeTextureCreation(
                    device, desc, initial_data, out_texture);
            }
        } else if (scale_this) {
            return RetryNativeTextureCreation(
                device, desc, initial_data, out_texture);
        }
    }
    return result;
}

// Device-vtable slot 15 (CreatePixelShader): the former ReShade
// create_pipeline/init_pipeline event pair collapses into this detour. The
// game's bytecode pointer is never modified: matching runs on the caller's
// buffer, the scaled constants go into a LOCAL patched copy, the DXBC
// checksum is recomputed, and the copy is validated with a probe create
// through the original function before the real shader is created from it.
// A failed match, patch, or validation forwards the game's own bytecode
// untouched (fail-closed). The detour also keeps the census role of the old
// hook: every pixel shader's DXBC identity is recorded (census debug mode)
// so draw-time bindings can be attributed to a dumped shader file
// (ps-CRC-size.dxbc).
using CreatePixelShaderFunction = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
    ID3D11PixelShader**);

// Exact DXBC identity match against the captured shadow-shader table:
// CRC-32 + blob size + the captured DXBC checksum bytes. The CRC is
// precomputed by the caller (it doubles as the census identity).
const ShadowShaderId* MatchShadowShader(const void* code,
                                        std::size_t size,
                                        std::uint32_t crc32_value) noexcept {
    if (!code || size < 64) {
        return nullptr;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(code);
    if (std::memcmp(bytes, "DXBC", 4) != 0) {
        return nullptr;
    }
    for (const ShadowShaderId& id : kShadowShaders) {
        if (id.crc32 == crc32_value && id.size == size &&
            std::memcmp(bytes + 4, id.dxbc_checksum.data(),
                        id.dxbc_checksum.size()) == 0) {
            return &id;
        }
    }
    return nullptr;
}

void DumpShaderIfEnabled(DeviceData* data,
                         const void* code, std::size_t size) noexcept;

HRESULT STDMETHODCALLTYPE DetourCreatePixelShader(
    ID3D11Device* device,
    const void* bytecode,
    SIZE_T bytecode_size,
    ID3D11ClassLinkage* class_linkage,
    ID3D11PixelShader** out_shader) {
    NativeHookCallGuard hook_call;
    DeviceData* const data = hook_call.accepted() ? g_device_data : nullptr;
    const bool identity_needed =
        data != nullptr &&
        (data->settings.enabled || data->settings.census_consumers ||
         data->settings.dump_shaders);
    std::uint32_t crc = 0;
    const ShadowShaderId* matched = nullptr;
    if (identity_needed && bytecode && bytecode_size >= 64) {
        const auto* bytes = static_cast<const std::uint8_t*>(bytecode);
        if (std::memcmp(bytes, "DXBC", 4) == 0) {
            crc = ComputeCrc32(bytes, bytecode_size);
            if (data->settings.enabled) {
                matched = MatchShadowShader(bytecode, bytecode_size, crc);
            }
        }
    }
    const void* effective_code = bytecode;
    SIZE_T effective_size = bytecode_size;
    // Lives past the create call below: effective_code may point into this
    // buffer after the patch block ends, so it must not be scoped to the try.
    std::vector<std::uint8_t> patched;
    if (matched != nullptr) {
        try {
            data->exact_identities_observed.fetch_add(
                1, std::memory_order_relaxed);
            // Per-shader atlas: a shader that filters a 2048 map (the
            // character-map class) is already at native density and is left
            // untouched (doc-280 per-shader map_override).
            const std::uint32_t filter_atlas =
                matched->map_override != 0 ? matched->map_override
                                           : data->settings.atlas_size;
            std::size_t changed = 0;
            if (PatchShadowShaderConstants(
                    static_cast<const std::uint8_t*>(bytecode), bytecode_size,
                    filter_atlas, patched, &changed)) {
                // Recompute the DXBC checksum BEFORE validation: the D3D11
                // runtime validates it at CreatePixelShader and rejects any
                // blob with a stale checksum (E_INVALIDARG).
                std::uint8_t checksum[16];
                checksum::Compute(patched.data(), patched.size(), checksum);
                std::memcpy(patched.data() + 4, checksum, 16);
                // Validate the patched bytecode against the D3D11 runtime
                // through the ORIGINAL function before installing it. A
                // structurally invalid patch leaves the native shader
                // untouched (fail-closed, same component-scoped guard).
                ComPtr<ID3D11PixelShader> probe;
                const HRESULT validation_result =
                    reinterpret_cast<CreatePixelShaderFunction>(
                        g_original_create_pixel_shader)(
                        device, patched.data(), patched.size(), class_linkage,
                        probe.ReleaseAndGetAddressOf());
                if (FAILED(validation_result) || !probe) {
                    if (!data->logged_identity_failure.exchange(
                            true, std::memory_order_relaxed)) {
                        Log(kLogError,
                            "[SPatch-ShadowScale] Driver rejected the patched shadow shader 0x%08X (HRESULT=0x%08X); retaining the native shader.",
                            matched->crc32,
                            static_cast<unsigned int>(validation_result));
                    }
                } else {
                    probe.Reset();
                    effective_code = patched.data();
                    effective_size = patched.size();
                    // The census key uses the identity of the bytecode that
                    // actually created the shader (post-patch for patched
                    // shaders).
                    crc = ComputeCrc32(patched.data(), patched.size());
                    const std::uint64_t count =
                        data->patched_shaders.fetch_add(
                            1, std::memory_order_relaxed) + 1;
                    Log(kLogInfo,
                        "[SPatch-ShadowScale] Patched shadow shader 0x%08X for the %u atlas: %zu constants scaled (total=%llu).",
                        matched->crc32, filter_atlas, changed,
                        static_cast<unsigned long long>(count));
                }
            } else if (filter_atlas > 2048 &&
                       !data->logged_identity_failure.exchange(
                           true, std::memory_order_relaxed)) {
                Log(kLogWarning,
                    "[SPatch-ShadowScale] Shadow shader 0x%08X matched but produced no patchable constants; retaining the native shader.",
                    matched->crc32);
            }
        } catch (...) {
            effective_code = bytecode;
            effective_size = bytecode_size;
            if (!data->logged_pipeline_exception.exchange(
                    true, std::memory_order_relaxed)) {
                Log(kLogError,
                    "[SPatch-ShadowScale] Shadow shader bookkeeping allocation failed; retaining the native shader.");
            }
        }
    }
    DumpShaderIfEnabled(data, effective_code, effective_size);
    const HRESULT result = reinterpret_cast<CreatePixelShaderFunction>(
        g_original_create_pixel_shader)(
        device, effective_code, effective_size, class_linkage, out_shader);
    if (data != nullptr && data->settings.census_consumers &&
        SUCCEEDED(result) && out_shader != nullptr && *out_shader != nullptr &&
        effective_code != nullptr && effective_size >= 64 &&
        std::memcmp(static_cast<const std::uint8_t*>(effective_code), "DXBC",
                    4) == 0) {
        try {
            std::lock_guard<std::mutex> lock(data->census_mutex);
            if (data->ps_crc.size() < 6000) {
                data->ps_crc.insert_or_assign(*out_shader, crc);
            }
        } catch (...) {
            // Shader identity tracking is diagnostic only.
        }
    }
    return result;
}

void STDMETHODCALLTYPE DetourPsSetShader(
    ID3D11DeviceContext* context,
    ID3D11PixelShader* pixel_shader,
    ID3D11ClassInstance* const* class_instances,
    UINT num_class_instances) {
    NativeHookCallGuard hook_call;
    if (hook_call.accepted()) {
        StateFor(context).current_ps = pixel_shader;
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, ID3D11PixelShader*,
        ID3D11ClassInstance* const*, UINT)>(OriginalFor(
            context, &ContextHookSet::ps_set_shader))(
        context, pixel_shader, class_instances, num_class_instances);
}

// Census: when a bound SRV's resource is one of the scaled shadow maps,
// record the (pixel shader, native map, scaled map) triple. Runs on both
// the immediate and the deferred-context vtables, so every path that binds
// a shadow map is seen. Diagnostic only; the scan is skipped entirely when
// the census debug key is off.
void STDMETHODCALLTYPE DetourPsSetShaderResources(
    ID3D11DeviceContext* context,
    UINT start_slot,
    UINT num_views,
    ID3D11ShaderResourceView* const* views) {
    NativeHookCallGuard hook_call;
    DeviceData* const data = hook_call.accepted() ? g_device_data : nullptr;
    ContextState* const state = data ? &StateFor(context) : nullptr;
    // Maintain the per-context PS-SRV resource set (exclusivity guard, always
    // on): a map bound as DSV while sampled as SRV is a consumer pass.
    if (data && views && num_views > 0 && start_slot < 32) {
        const std::size_t tracked_start = start_slot;
        const std::size_t tracked_count = (std::min)(
            static_cast<std::size_t>(num_views),
            std::size(state->ps_srvs) - tracked_start);
        for (std::size_t offset = 0; offset < tracked_count; ++offset) {
            const std::size_t slot = tracked_start + offset;
            if (slot >= std::size(state->ps_srvs)) {
                break;
            }
            ID3D11ShaderResourceView* const srv = views[offset];
            ComPtr<ID3D11Resource> resource;
            if (srv) {
                srv->GetResource(resource.ReleaseAndGetAddressOf());
            }
            state->ps_srvs.at(slot) = resource.Get();
        }
    }
    if (data && data->settings.census_consumers && views &&
        num_views > 0 && start_slot < 32 && state->current_ps) {
        const std::size_t tracked_start = start_slot;
        const std::size_t tracked_count = (std::min)(
            static_cast<std::size_t>(num_views),
            std::size(state->ps_srvs) - tracked_start);
        for (std::size_t offset = 0; offset < tracked_count; ++offset) {
            ID3D11ShaderResourceView* const srv = views[offset];
            if (!srv) {
                continue;
            }
            ComPtr<ID3D11Resource> resource;
            srv->GetResource(resource.ReleaseAndGetAddressOf());
            if (!resource) {
                continue;
            }
            std::uint32_t native = 0;
            std::uint32_t scaled = 0;
            const bool matched =
                LookupScaledTarget(resource.Get(), native, scaled);
            if (!matched) {
                continue;
            }
            try {
                std::lock_guard<std::mutex> lock(data->census_mutex);
                std::uint32_t crc = 0;
                const auto it = data->ps_crc.find(state->current_ps);
                if (it != data->ps_crc.end()) {
                    crc = it->second;
                }
                const std::uint64_t key =
                    (static_cast<std::uint64_t>(crc) << 32) |
                    (static_cast<std::uint64_t>(native) << 16) |
                    scaled;
                if (data->census_counts.size() < 512 ||
                    data->census_counts.count(key) != 0) {
                    ++data->census_counts[key];
                }
            } catch (...) {
                // Capped consumer counts are diagnostic only.
            }
        }
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*)>(
        OriginalFor(context, &ContextHookSet::ps_set_shader_resources))(
            context, start_slot, num_views, views);
}

void STDMETHODCALLTYPE DetourOmSetRenderTargets(
    ID3D11DeviceContext* context,
    UINT num_views,
    ID3D11RenderTargetView* const* render_target_views,
    ID3D11DepthStencilView* depth_stencil_view) {
    NativeHookCallGuard hook_call;
    // An all-null render-target array is the classic D3D11 idiom for "no
    // color output" (the game binds it together with shadow-map DSVs), so it
    // counts as depth-only alongside the num_views == 0 form. The array
    // itself must be non-null before it is walked (defensive: some engines
    // pass a null array with a nonzero count).
    bool all_null = true;
    if (render_target_views != nullptr) {
        for (UINT i = 0; i < num_views; ++i) {
            if (render_target_views[i] != nullptr) {
                all_null = false;
                break;
            }
        }
    } else if (num_views > 0) {
        all_null = false;  // unverifiable -> treat as not depth-only
    }
    if (hook_call.accepted()) {
        TrackShadowDsv(context, depth_stencil_view,
                       num_views == 0 || all_null, num_views, 0);
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
        ID3D11DepthStencilView*)>(OriginalFor(
            context, &ContextHookSet::om_set_render_targets))(
        context, num_views, render_target_views, depth_stencil_view);
}

void STDMETHODCALLTYPE DetourOmSetRenderTargetsAndUavs(
    ID3D11DeviceContext* context,
    UINT num_rtvs,
    ID3D11RenderTargetView* const* render_target_views,
    ID3D11DepthStencilView* depth_stencil_view,
    UINT uav_start_slot,
    UINT num_uavs,
    ID3D11UnorderedAccessView* const* unordered_access_views,
    const UINT* initial_counts) {
    NativeHookCallGuard hook_call;
    // D3D11 uses UINT_MAX sentinels to preserve the existing RTV/DSV or UAV
    // bindings. Never walk those values as array lengths, and retain the
    // current DSV tracker when the call explicitly keeps it unchanged.
    const bool keep_render_targets =
        num_rtvs == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL;
    const bool keep_unordered_access_views =
        num_uavs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS;
    bool rtvs_all_null = true;
    if (!keep_render_targets && render_target_views != nullptr) {
        for (UINT i = 0; i < num_rtvs; ++i) {
            if (render_target_views[i] != nullptr) {
                rtvs_all_null = false;
                break;
            }
        }
    } else if (!keep_render_targets && num_rtvs > 0) {
        rtvs_all_null = false;  // unverifiable -> treat as not depth-only
    }
    bool uavs_all_null = true;
    if (!keep_unordered_access_views && unordered_access_views != nullptr) {
        for (UINT i = 0; i < num_uavs; ++i) {
            if (unordered_access_views[i] != nullptr) {
                uavs_all_null = false;
                break;
            }
        }
    } else if (keep_unordered_access_views || num_uavs > 0) {
        uavs_all_null = false;  // unverifiable -> treat as not depth-only
    }
    if (hook_call.accepted() && !keep_render_targets) {
        TrackShadowDsv(
            context, depth_stencil_view,
            (num_rtvs == 0 || rtvs_all_null) &&
                (num_uavs == 0 || uavs_all_null),
            num_rtvs, num_uavs);
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
        ID3D11DepthStencilView*, UINT, UINT,
        ID3D11UnorderedAccessView* const*, const UINT*)>(
        OriginalFor(context, &ContextHookSet::om_set_render_targets_and_uavs))(
        context, num_rtvs, render_target_views, depth_stencil_view,
        uav_start_slot, num_uavs, unordered_access_views, initial_counts);
}

// True when the scaled map bound as DSV is also bound as a PS SRV on this
// thread. D3D11 forbids SRV+DSV on one resource/context simultaneously, so a
// genuine shadow render pass (writing the map) never trips this; a consumer
// pass that keeps a stale shadow DSV bound while sampling the map does.
bool MapBoundAsPsSrv(const ContextState& state,
                     ID3D11Resource* resource) noexcept {
    for (ID3D11Resource* const bound : state.ps_srvs) {
        if (bound == resource) {
            return true;
        }
    }
    return false;
}

// Census: log the first few viewport corrections / consumer blocks
// with the bound pixel shader identity (post-patch CRC for patched
// shaders). Diagnostic only; capped so the hot path stays quiet.
void LogCensusEvent(ID3D11DeviceContext* context, DeviceData* data,
                    std::atomic<std::uint32_t>& counter,
                    LogLevel level, const char* what,
                    std::uint32_t native, std::uint32_t scaled) noexcept {
    if (!data->settings.census_consumers) {
        return;  // keep the census-off path clean
    }
    const std::uint32_t n = counter.fetch_add(1, std::memory_order_relaxed);
    if (n >= 8) {
        return;
    }
    std::uint32_t crc = 0;
    ID3D11PixelShader* const current_ps = StateFor(context).current_ps;
    if (current_ps) {
        try {
            std::lock_guard<std::mutex> lock(data->census_mutex);
            const auto it = data->ps_crc.find(current_ps);
            if (it != data->ps_crc.end()) {
                crc = it->second;
            }
        } catch (...) {
            // The capped event remains useful with an unknown shader ID.
        }
    }
    Log(level, "[SPatch-ShadowScale] %s: map %u->%u PS=0x%08X.",
        what, native, scaled, crc);
}

void STDMETHODCALLTYPE DetourRsSetViewports(
    ID3D11DeviceContext* context,
    UINT num_viewports,
    const D3D11_VIEWPORT* viewports) {
    NativeHookCallGuard hook_call;
    DeviceData* const data = hook_call.accepted() ? g_device_data : nullptr;
    if (!data) {
        reinterpret_cast<void(STDMETHODCALLTYPE*)(
            ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*)>(
            OriginalFor(context, &ContextHookSet::rs_set_viewports))(
                context, num_viewports, viewports);
        return;
    }
    ContextState& state = StateFor(context);
    // Snapshot for the draw-time correction regardless of ordering.
    CurrentViewports& snapshot = state.viewports;
    const UINT stored_count = (std::min)(
        num_viewports,
        static_cast<UINT>(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE));
    for (UINT i = 0; i < stored_count; ++i) {
        snapshot.values[i] = viewports[i];
    }
    snapshot.count = stored_count;

    const CurrentShadowDsv& current = state.shadow_dsv;
    if (current.resource && current.scaled_size != current.native_size &&
        MapBoundAsPsSrv(state, current.resource)) {
        // Consumer pass: the map is being sampled, not rendered into. The
        // bound DSV is stale depth-testing state, so the viewport must stay
        // at its native size (scaling it corrupted lighting in the runtime probe).
        data->srv_blocked_corrections.fetch_add(
            1, std::memory_order_relaxed);
        LogCensusEvent(context, data, data->srv_blocked_logged,
                       kLogWarning,
                       "Consumer pass: DSV+SRV, viewport left at native",
                       current.native_size, current.scaled_size);
    }
    if (data && current.resource && current.scaled_size != current.native_size &&
        num_viewports > 0 && viewports &&
        !MapBoundAsPsSrv(state, current.resource)) {
        const UINT count = stored_count;
        D3D11_VIEWPORT scaled[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        bool changed = false;
        for (UINT i = 0; i < count; ++i) {
            scaled[i] = viewports[i];
            if (scaled[i].Width == scaled[i].Height &&
                scaled[i].Width == static_cast<float>(current.native_size)) {
                const float factor = static_cast<float>(current.scaled_size) /
                                     static_cast<float>(current.native_size);
                scaled[i].Width *= factor;
                scaled[i].Height *= factor;
                changed = true;
            }
            // If the game already derived the scaled size from the scaled map
            // descriptor, the viewport matches scaled_size and is forwarded
            // untouched to avoid double scaling.
        }
        if (changed) {
            for (UINT i = 0; i < count; ++i) {
                snapshot.values[i] = scaled[i];
            }
            data->scaled_viewports.fetch_add(1, std::memory_order_relaxed);
            LogCensusEvent(context, data, data->correction_logged,
                           kLogInfo,
                           "Scaled shadow-pass viewport",
                           current.native_size, current.scaled_size);
            CountViewportCorrection(data, current.native_size, current.scaled_size);
            reinterpret_cast<void(STDMETHODCALLTYPE*)(
                ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*)>(
                OriginalFor(context, &ContextHookSet::rs_set_viewports))(
                    context, count, scaled);
            return;
        }
        if (viewports[0].Width == static_cast<float>(current.scaled_size) &&
            viewports[0].Height == static_cast<float>(current.scaled_size)) {
            if (!data->logged_already_scaled_viewport.exchange(
                    true, std::memory_order_relaxed)) {
                Log(kLogInfo,
                    "[SPatch-ShadowScale] Shadow-pass viewport already matches the scaled map (%ux%u, map %u->%u); game derived it from the map descriptor.",
                    static_cast<unsigned>(viewports[0].Width),
                    static_cast<unsigned>(viewports[0].Height),
                    current.native_size, current.scaled_size);
            }
        }
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*)>(
        OriginalFor(context, &ContextHookSet::rs_set_viewports))(
            context, num_viewports, viewports);
}

// Draw-time correction: when the bound shadow DSV belongs to a scaled map but
// the viewport is still at the native size (the game set the viewport before
// binding the DSV), re-apply the viewport at the scaled size immediately
// before the draw so the pass renders the full scaled map. Idempotent: after
// the first correction the snapshot holds the scaled sizes.
void EnsureShadowViewportScaled(ID3D11DeviceContext* context) noexcept {
    ContextState& state = StateFor(context);
    const CurrentShadowDsv& dsv = state.shadow_dsv;
    if (!dsv.resource || dsv.scaled_size == dsv.native_size) {
        return;
    }
    if (MapBoundAsPsSrv(state, dsv.resource) && g_device_data) {
        // Consumer pass at draw time: the map is sampled, not rendered into.
        g_device_data->srv_blocked_corrections.fetch_add(
            1, std::memory_order_relaxed);
        LogCensusEvent(context, g_device_data, g_device_data->srv_blocked_logged,
                       kLogWarning,
                       "Consumer pass at draw time: DSV+SRV",
                       dsv.native_size, dsv.scaled_size);
        return;
    }
    CurrentViewports& snapshot = state.viewports;
    if (snapshot.count == 0) {
        return;
    }
    D3D11_VIEWPORT scaled[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    bool matches_native = false;
    for (UINT i = 0; i < snapshot.count; ++i) {
        scaled[i] = snapshot.values[i];
        if (scaled[i].Width == scaled[i].Height &&
            scaled[i].Width == static_cast<float>(dsv.native_size)) {
            const float factor = static_cast<float>(dsv.scaled_size) /
                                 static_cast<float>(dsv.native_size);
            scaled[i].Width *= factor;
            scaled[i].Height *= factor;
            matches_native = true;
        }
    }
    if (!matches_native) {
        return;
    }
    g_device_data->scaled_viewports.fetch_add(1, std::memory_order_relaxed);
    LogCensusEvent(context, g_device_data, g_device_data->correction_logged,
                   kLogInfo,
                   "Scaled shadow-pass viewport at draw time",
                   dsv.native_size, dsv.scaled_size);
    CountViewportCorrection(g_device_data, dsv.native_size, dsv.scaled_size);
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*)>(
        OriginalFor(context, &ContextHookSet::rs_set_viewports))(
            context, snapshot.count, scaled);
    for (UINT i = 0; i < snapshot.count; ++i) {
        snapshot.values[i] = scaled[i];
    }
}

void STDMETHODCALLTYPE DetourDraw(
    ID3D11DeviceContext* context, UINT vertex_count, UINT start_vertex) {
    NativeHookCallGuard hook_call;
    if (hook_call.accepted()) {
        EnsureShadowViewportScaled(context);
        RecordShadowRenderIfCensus(context);
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, UINT)>(OriginalFor(
            context, &ContextHookSet::draw))(
        context, vertex_count, start_vertex);
}

void STDMETHODCALLTYPE DetourDrawIndexed(
    ID3D11DeviceContext* context, UINT index_count, UINT start_index, INT base_vertex) {
    NativeHookCallGuard hook_call;
    if (hook_call.accepted()) {
        EnsureShadowViewportScaled(context);
        RecordShadowRenderIfCensus(context);
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, UINT, INT)>(OriginalFor(
            context, &ContextHookSet::draw_indexed))(
        context, index_count, start_index, base_vertex);
}

void STDMETHODCALLTYPE DetourDrawInstanced(
    ID3D11DeviceContext* context, UINT vertex_count, UINT instance_count,
    UINT start_vertex, UINT start_instance) {
    NativeHookCallGuard hook_call;
    if (hook_call.accepted()) {
        EnsureShadowViewportScaled(context);
        RecordShadowRenderIfCensus(context);
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, UINT, UINT, UINT)>(OriginalFor(
            context, &ContextHookSet::draw_instanced))(
        context, vertex_count, instance_count, start_vertex, start_instance);
}

void STDMETHODCALLTYPE DetourDrawIndexedInstanced(
    ID3D11DeviceContext* context, UINT index_count, UINT instance_count,
    UINT start_index, INT base_vertex, UINT start_instance) {
    NativeHookCallGuard hook_call;
    if (hook_call.accepted()) {
        EnsureShadowViewportScaled(context);
        RecordShadowRenderIfCensus(context);
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT)>(
        OriginalFor(context, &ContextHookSet::draw_indexed_instanced))(
        context, index_count, instance_count, start_index, base_vertex, start_instance);
}

void STDMETHODCALLTYPE DetourDrawIndexedInstancedIndirect(
    ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset) {
    NativeHookCallGuard hook_call;
    if (hook_call.accepted()) {
        EnsureShadowViewportScaled(context);
        RecordShadowRenderIfCensus(context);
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, ID3D11Buffer*, UINT)>(
        OriginalFor(context, &ContextHookSet::draw_indexed_instanced_indirect))(
            context, args, offset);
}

void STDMETHODCALLTYPE DetourDrawInstancedIndirect(
    ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset) {
    NativeHookCallGuard hook_call;
    if (hook_call.accepted()) {
        EnsureShadowViewportScaled(context);
        RecordShadowRenderIfCensus(context);
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, ID3D11Buffer*, UINT)>(
        OriginalFor(context, &ContextHookSet::draw_instanced_indirect))(
            context, args, offset);
}

void STDMETHODCALLTYPE DetourDrawAuto(ID3D11DeviceContext* context) {
    NativeHookCallGuard hook_call;
    if (hook_call.accepted()) {
        EnsureShadowViewportScaled(context);
        RecordShadowRenderIfCensus(context);
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*)>(
        OriginalFor(context, &ContextHookSet::draw_auto))(context);
}

void STDMETHODCALLTYPE DetourClearState(ID3D11DeviceContext* context) {
    NativeHookCallGuard hook_call;
    if (hook_call.accepted()) {
        ContextState& state = StateFor(context);
        const std::uint64_t epoch = state.epoch;
        state = ContextState{};
        state.epoch = epoch;
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*)>(OriginalFor(
            context, &ContextHookSet::clear_state))(context);
}

// Executing a command list applies the recorded OM/viewport/SRV state to
// the immediate context without any of the detoured bind calls firing,
// leaving the thread-local tracking stale. A later viewport that happens
// to match a shadow map's native size would then be wrongly scaled on a
// non-shadow pass, which the fullscreen runtime probe showed corrupts lighting.
// Clear all tracking so a correction can only fire after a freshly
// observed depth-only bind.
void STDMETHODCALLTYPE DetourExecuteCommandList(
    ID3D11DeviceContext* context,
    ID3D11CommandList* command_list,
    BOOL restore_context_state) {
    NativeHookCallGuard hook_call;
    ContextState saved_state;
    if (hook_call.accepted()) {
        ContextState& state = StateFor(context);
        saved_state = state;
        const std::uint64_t epoch = state.epoch;
        state = ContextState{};
        state.epoch = epoch;
    }
    if (hook_call.accepted() && g_device_data) {
        g_device_data->executed_command_lists.fetch_add(1, std::memory_order_relaxed);
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, ID3D11CommandList*, BOOL)>(
        OriginalFor(context, &ContextHookSet::execute_command_list))(
        context, command_list, restore_context_state);
    if (hook_call.accepted() && restore_context_state) {
        // ExecuteCommandList restores the immediate context's prior state;
        // mirror that contract instead of leaving our tracker at defaults.
        StateFor(context) = saved_state;
    }
}

HRESULT STDMETHODCALLTYPE DetourFinishCommandList(
    ID3D11DeviceContext* context,
    BOOL restore_deferred_context_state,
    ID3D11CommandList** command_list) {
    NativeHookCallGuard hook_call;
    const HRESULT result = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(
        ID3D11DeviceContext*, BOOL, ID3D11CommandList**)>(
        OriginalFor(context, &ContextHookSet::finish_command_list))(
        context, restore_deferred_context_state, command_list);
    if (hook_call.accepted() && SUCCEEDED(result) &&
        !restore_deferred_context_state) {
        // FinishCommandList resets a deferred context to defaults in this
        // mode. Carrying its old DSV/viewport/SRV state into the next recording
        // could scale an unrelated pass on the same worker thread.
        ContextState& state = StateFor(context);
        const std::uint64_t epoch = state.epoch;
        state = ContextState{};
        state.epoch = epoch;
    }
    return result;
}

bool PinNativeHookModule() noexcept {
    if (g_native_module_pinned.load(std::memory_order_acquire)) {
        return true;
    }
    if (g_module == nullptr) {
        return false;
    }
    HMODULE pinned_module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(g_module), &pinned_module)) {
        Log(kLogError,
            "[SPatch-ShadowScale] native hooks rejected because the add-on "
            "image could not be pinned for process lifetime (error=%lu).",
            GetLastError());
        return false;
    }
    g_native_module_pinned.store(true, std::memory_order_release);
    return true;
}

bool DrainAcceptedNativeHookCalls() noexcept {
    constexpr ULONGLONG kDrainTimeoutMilliseconds = 5000;
    const ULONGLONG deadline = GetTickCount64() + kDrainTimeoutMilliseconds;
    while (g_native_hook_accepted_calls.load(std::memory_order_acquire) != 0) {
        if (GetTickCount64() >= deadline) {
            return false;
        }
        if (!SwitchToThread()) {
            Sleep(1);
        }
    }
    return true;
}

bool DeactivateNativeHooks() noexcept {
    g_native_hook_accepting.store(false, std::memory_order_release);
    const bool drained = DrainAcceptedNativeHookCalls();
    if (!drained) {
        Log(kLogWarning,
            "[SPatch-ShadowScale] native hook call drain timed out; "
            "behavior remains disabled and device state is retained.");
        return false;
    }
    if (g_device_data) {
        g_device_data->settings.enabled = false;
        g_device_data->settings.texture_scale = 0.0f;
        g_device_data->settings.atlas_size = 0;
    }
    g_native_device = nullptr;
    g_device_data = nullptr;
    g_context_state_epoch.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

// Installs the context-method detours on one context vtable (immediate or
// the shared deferred-class vtable). Slot indices match the D3D11
// ID3D11DeviceContext vtable (OMSetRenderTargets=33,
// OMSetRenderTargetsAndUAVs=34, RSSetViewports=44, PSSetShaderResources=8,
// PSSetShader=9, DrawIndexed=12, Draw=13, DrawIndexedInstanced=20,
// DrawInstanced=21, DrawAuto=38, DrawIndexedInstancedIndirect=39,
// DrawInstancedIndirect=40, ExecuteCommandList=58, ClearState=110,
// FinishCommandList=114).
bool InstallContextHooks(void* vtable, ContextHookSet& hooks) {
    void** const slots = static_cast<void**>(vtable);
    // Deferred contexts return the same vtable pointer as the immediate
    // context, so once the immediate set is installed every deferred call
    // already goes through those detours; publishing the same trampolines
    // into this set (via OriginalFor's fallback) is all that is needed.
    // MinHook rejects a second MH_CreateHook on the same target
    // (MH_ERROR_ALREADY_CREATED), so re-installing would fail closed.
    if (vtable == static_cast<void*>(g_immediate_vtable) &&
        &hooks != &g_immediate_hooks) {
        hooks = g_immediate_hooks;
        return true;
    }
    // Some deferred-context vtable slots point to the exact same function
    // addresses as the immediate-context vtable (the runtime shares those
    // implementations). Those are already hooked through the immediate set,
    // and the detour fires regardless of which context makes the call, so
    // they must be skipped - MinHook rejects the duplicate target.
    const bool skip_shared = vtable != static_cast<void*>(g_immediate_vtable);
    void** const shared = skip_shared ? static_cast<void**>(g_immediate_vtable)
                                      : nullptr;
    const auto install = [](int slot, void* target, void* detour,
                           void** original) -> bool {
        if (!target || !original) {
            Log(kLogError,
                "[SPatch-ShadowScale] Hook install failed at context slot %d (null target).",
                slot);
            return false;
        }
        if (*original != nullptr) {
            return true;
        }
        // The pinned MinHook fork enables the target before MH_CreateHook
        // returns. Publish the trampoline into the exact slot used by the
        // detour so a resumed D3D thread can never observe a null original.
        const int status = static_cast<int>(
            MH_CreateHook(target, detour, original));
        const bool ok = status == static_cast<int>(MH_OK);
        if (!ok) {
            Log(kLogError,
                "[SPatch-ShadowScale] Hook install failed at context slot %d (target=%p) MH=%d.",
                slot, target, status);
        }
        return ok;
    };
    bool ok = true;
    if (!skip_shared || !shared || shared[33] != slots[33]) {
        ok &= install(33, slots[33], reinterpret_cast<void*>(&DetourOmSetRenderTargets),
                      &hooks.om_set_render_targets);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.om_set_render_targets = g_immediate_hooks.om_set_render_targets;
    }
    if (!skip_shared || !shared || shared[34] != slots[34]) {
        ok &= install(34, slots[34], reinterpret_cast<void*>(&DetourOmSetRenderTargetsAndUavs),
                      &hooks.om_set_render_targets_and_uavs);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.om_set_render_targets_and_uavs = g_immediate_hooks.om_set_render_targets_and_uavs;
    }
    if (!skip_shared || !shared || shared[44] != slots[44]) {
        ok &= install(44, slots[44], reinterpret_cast<void*>(&DetourRsSetViewports),
                      &hooks.rs_set_viewports);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.rs_set_viewports = g_immediate_hooks.rs_set_viewports;
    }
    if (!skip_shared || !shared || shared[8] != slots[8]) {
        ok &= install(8, slots[8], reinterpret_cast<void*>(&DetourPsSetShaderResources),
                      &hooks.ps_set_shader_resources);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.ps_set_shader_resources = g_immediate_hooks.ps_set_shader_resources;
    }
    if (!skip_shared || !shared || shared[9] != slots[9]) {
        ok &= install(9, slots[9], reinterpret_cast<void*>(&DetourPsSetShader),
                      &hooks.ps_set_shader);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.ps_set_shader = g_immediate_hooks.ps_set_shader;
    }
    if (!skip_shared || !shared || shared[13] != slots[13]) {
        ok &= install(13, slots[13], reinterpret_cast<void*>(&DetourDraw),
                      &hooks.draw);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.draw = g_immediate_hooks.draw;
    }
    if (!skip_shared || !shared || shared[12] != slots[12]) {
        ok &= install(12, slots[12], reinterpret_cast<void*>(&DetourDrawIndexed),
                      &hooks.draw_indexed);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.draw_indexed = g_immediate_hooks.draw_indexed;
    }
    if (!skip_shared || !shared || shared[21] != slots[21]) {
        ok &= install(21, slots[21], reinterpret_cast<void*>(&DetourDrawInstanced),
                      &hooks.draw_instanced);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.draw_instanced = g_immediate_hooks.draw_instanced;
    }
    if (!skip_shared || !shared || shared[20] != slots[20]) {
        ok &= install(20, slots[20], reinterpret_cast<void*>(&DetourDrawIndexedInstanced),
                      &hooks.draw_indexed_instanced);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.draw_indexed_instanced = g_immediate_hooks.draw_indexed_instanced;
    }
    if (!skip_shared || !shared || shared[38] != slots[38]) {
        ok &= install(38, slots[38], reinterpret_cast<void*>(&DetourDrawAuto),
                      &hooks.draw_auto);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.draw_auto = g_immediate_hooks.draw_auto;
    }
    if (!skip_shared || !shared || shared[39] != slots[39]) {
        ok &= install(39, slots[39], reinterpret_cast<void*>(&DetourDrawIndexedInstancedIndirect),
                      &hooks.draw_indexed_instanced_indirect);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.draw_indexed_instanced_indirect = g_immediate_hooks.draw_indexed_instanced_indirect;
    }
    if (!skip_shared || !shared || shared[40] != slots[40]) {
        ok &= install(40, slots[40], reinterpret_cast<void*>(&DetourDrawInstancedIndirect),
                      &hooks.draw_instanced_indirect);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.draw_instanced_indirect = g_immediate_hooks.draw_instanced_indirect;
    }
    if (!skip_shared || !shared || shared[58] != slots[58]) {
        ok &= install(58, slots[58], reinterpret_cast<void*>(&DetourExecuteCommandList),
                      &hooks.execute_command_list);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.execute_command_list = g_immediate_hooks.execute_command_list;
    }
    if (!skip_shared || !shared || shared[110] != slots[110]) {
        ok &= install(110, slots[110], reinterpret_cast<void*>(&DetourClearState),
                      &hooks.clear_state);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and the detour
        // routes through OriginalFor, so this set needs the same original
        // to forward deferred-context calls correctly.
        hooks.clear_state = g_immediate_hooks.clear_state;
    }
    if (!skip_shared || !shared || shared[114] != slots[114]) {
        ok &= install(114, slots[114], reinterpret_cast<void*>(&DetourFinishCommandList),
                      &hooks.finish_command_list);
    } else {
        // The deferred vtable shares this function address with the
        // immediate context; it is already hooked there and OriginalFor routes
        // calls to this set's matching trampoline.
        hooks.finish_command_list = g_immediate_hooks.finish_command_list;
    }
    return ok;
}

bool ContextHookTargetsMatch(void* candidate_vtable,
                             void* installed_vtable) noexcept {
    if (candidate_vtable == nullptr || installed_vtable == nullptr) {
        return false;
    }
    void** const candidate = static_cast<void**>(candidate_vtable);
    void** const installed = static_cast<void**>(installed_vtable);
    for (const int slot :
         {33, 34, 44, 8, 9, 13, 12, 21, 20, 38, 39, 40, 58, 110, 114}) {
        if (candidate[slot] != installed[slot]) {
            return false;
        }
    }
    return true;
}

bool InstallNativeHooks(ID3D11Device* native) {
    DeviceData* const data = &g_data;
    if (!native) {
        return false;
    }
    // Hooks and their trampolines are process-lifetime. Close the behavioral
    // gate and drain users of the previous device before changing the active
    // data pointer.
    if (!DeactivateNativeHooks() || !PinNativeHookModule()) {
        data->settings.enabled = false;
        data->settings.texture_scale = 0.0f;
        data->settings.atlas_size = 0;
        return false;
    }
    void** const device_vtable = *reinterpret_cast<void***>(native);

    // The SDmodding MinHook build auto-initializes on the first MH_CreateHook.
    const auto install = [](int slot, void* target, void* detour,
                           void** original, void** installed_target) -> bool {
        if (target == nullptr || original == nullptr ||
            installed_target == nullptr) {
            Log(kLogError,
                "[SPatch-ShadowScale] Hook install failed at device slot %d (target=%p).",
                slot, target);
            return false;
        }
        if (*installed_target != nullptr) {
            if (*installed_target != target || *original == nullptr) {
                Log(kLogError,
                    "[SPatch-ShadowScale] Device slot %d changed target after a process-lifetime hook was installed; scaling remains disabled.",
                    slot);
                return false;
            }
            return true;
        }
        // The pinned MinHook fork enables the target before MH_CreateHook
        // returns. Write the trampoline directly to the detour-visible slot
        // before MinHook resumes threads.
        const MH_STATUS status = MH_CreateHook(target, detour, original);
        if (status != MH_OK) {
            Log(kLogError,
                "[SPatch-ShadowScale] Hook install failed at device slot %d (target=%p) MH=%d.",
                slot, target, static_cast<int>(status));
            return false;
        }
        *installed_target = target;
        return true;
    };
    bool ok = install(5, device_vtable[5], reinterpret_cast<void*>(&DetourCreateTexture2D),
                      &g_original_create_texture2d,
                      &g_hook_target_create_texture2d);
    ok &= install(15, device_vtable[15], reinterpret_cast<void*>(&DetourCreatePixelShader),
                  &g_original_create_pixel_shader,
                  &g_hook_target_create_pixel_shader);

    // Immediate context: without it the viewport correction cannot run; treat
    // it as a hard failure so maps are never scaled without viewport scaling.
    {
        ComPtr<ID3D11DeviceContext> context;
        native->GetImmediateContext(context.ReleaseAndGetAddressOf());
        if (!context) {
            ok = false;
            Log(kLogError,
                "[SPatch-ShadowScale] No immediate context; viewport correction unavailable.");
        } else {
            void* const candidate_vtable =
                *reinterpret_cast<void***>(context.Get());
            if (g_immediate_vtable == nullptr) {
                g_immediate_vtable = candidate_vtable;
            }
            const bool compatible = ContextHookTargetsMatch(
                candidate_vtable, g_immediate_vtable);
            const bool context_ok = compatible &&
                InstallContextHooks(g_immediate_vtable, g_immediate_hooks);
            ok &= context_ok;
            Log(kLogInfo,
                "[SPatch-ShadowScale] Immediate-context hooks: %s.",
                context_ok ? "ok" : "PARTIAL FAILURE");
        }
    }
    // Deferred contexts are a separate D3D11 class with one shared vtable;
    // hooking it covers every deferred instance (created before or after
    // init), so shadow passes recorded on deferred contexts also get the
    // viewport correction and census coverage. Both hook sets are required
    // because the game actively records and executes deferred command lists.
    {
        ComPtr<ID3D11DeviceContext> deferred;
        const HRESULT deferred_hr = native->CreateDeferredContext(
            0, deferred.ReleaseAndGetAddressOf());
        if (SUCCEEDED(deferred_hr)) {
            void* const candidate_vtable =
                *reinterpret_cast<void***>(deferred.Get());
            if (g_deferred_vtable == nullptr) {
                g_deferred_vtable = candidate_vtable;
            }
            const bool compatible = ContextHookTargetsMatch(
                candidate_vtable, g_deferred_vtable);
            const bool deferred_ok = compatible &&
                InstallContextHooks(g_deferred_vtable, g_deferred_hooks);
            // The game actively records and executes deferred command lists.
            // Partial coverage can resize a map without correcting all of its
            // recorded viewports, so this must fail closed just like the
            // immediate-context hook set.
            ok &= deferred_ok;
            Log(kLogInfo,
                "[SPatch-ShadowScale] Deferred-context hooks: %s.",
                deferred_ok ? "ok" : "PARTIAL FAILURE");
        } else {
            ok = false;
            Log(kLogWarning,
                "[SPatch-ShadowScale] CreateDeferredContext failed (HRESULT=0x%08X); disabling shadow-map scaling.",
                static_cast<unsigned int>(deferred_hr));
        }
    }
    if (!ok) {
        // Keep the game fully native: without map scaling the scaled filter
        // constants would not match the maps.
        data->settings.texture_scale = 0.0f;
        data->settings.atlas_size = 0;
        data->settings.enabled = false;
        Log(kLogError,
            "[SPatch-ShadowScale] Native hook installation failed; retaining native shadow maps and filters.");
        return false;
    }
    g_native_device = native;
    g_device_data = data;
    g_context_state_epoch.fetch_add(1, std::memory_order_acq_rel);
    g_native_hook_accepting.store(true, std::memory_order_release);
    Log(kLogInfo,
        "[SPatch-ShadowScale] Native shadow-map detours armed (device + immediate/deferred context hooks).");
    return true;
}

// ---------------------------------------------------------------------------
// Debug shader dump (DumpShaders=1): write every unique pixel shader DXBC the
// game compiles, so the baked texel constants of shadow-consuming shaders can
// be censused off-line. Only active with the debug key set.
// ---------------------------------------------------------------------------

void DumpShaderIfEnabled(DeviceData* data,
                         const void* code, std::size_t size) noexcept {
    if (!data->settings.dump_shaders || !code ||
        size < 64 || size > 2 * 1024 * 1024) {
        return;
    }
    try {
        const auto* bytes = static_cast<const std::uint8_t*>(code);
        if (std::memcmp(bytes, "DXBC", 4) != 0) {
            return;
        }
        const std::uint32_t crc = ComputeCrc32(bytes, size);
        const auto key = std::make_pair(crc, size);
        {
            std::lock_guard<std::mutex> lock(data->dump_mutex);
            if (data->dumped_shaders.size() >= 4000 ||
                !data->dumped_shaders.insert(key).second) {
                return;
            }
        }
        char name[64];
        std::snprintf(name, sizeof(name), "%08X-%zu.dxbc", crc, size);
        std::wstring wide_name;
        for (const char* p = name; *p; ++p) {
            wide_name += static_cast<wchar_t>(*p);
        }
        const std::wstring path =
            data->settings.dump_directory + L"ps-" + wide_name;
        FILE* file = nullptr;
        _wfopen_s(&file, path.c_str(), L"wb");
        if (!file) {
            return;
        }
        std::fwrite(bytes, 1, size, file);
        std::fclose(file);
        data->dumped_shader_files.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        // Shader dumping is diagnostic only and may allocate path/set storage.
    }
}

// ---------------------------------------------------------------------------
// Census reporting
// ---------------------------------------------------------------------------

// Logs the most frequent census entries (key = crc<<32 | native<<16 |
// scaled), most frequent first, up to 20.
void LogCensusTop(LogLevel level,
                  const std::unordered_map<std::uint64_t, std::uint64_t>& counts,
                  std::size_t limit = 20) noexcept {
    try {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted(
            counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const std::pair<std::uint64_t, std::uint64_t>& a,
                     const std::pair<std::uint64_t, std::uint64_t>& b) {
                      return a.second > b.second;
                  });
        limit = (std::min)(sorted.size(), limit);
        for (std::size_t i = 0; i < limit; ++i) {
            const std::uint64_t key = sorted[i].first;
            const std::uint32_t crc = static_cast<std::uint32_t>(key >> 32);
            const std::uint32_t native =
                static_cast<std::uint32_t>((key >> 16) & 0xFFFF);
            const std::uint32_t scaled =
                static_cast<std::uint32_t>(key & 0xFFFF);
            Log(level,
                "[SPatch-ShadowScale]   PS 0x%08X map %ux%u (%u->%u): %llu",
                crc, native, native, native, scaled,
                static_cast<unsigned long long>(sorted[i].second));
        }
    } catch (...) {
        // Sorting the optional census must not cross a ReShade callback.
    }
}

// Logs the current census state (totals, per-class viewport corrections,
// top render passes, top consumers). Called periodically from NoteFrame so
// the data survives any exit path (the game does not fire device loss on
// quit - verified 2026-08-06) and once more at device loss when that does
// fire.
void LogCensusSnapshot(DeviceData* data, std::size_t limit = 10) {
    Log(kLogInfo,
        "[SPatch-ShadowScale] Census snapshot: exact identities=%llu shaders patched=%llu maps scaled=%llu viewport corrections=%llu (srv-blocked=%llu, cmdlist-exec=%llu).",
        static_cast<unsigned long long>(data->exact_identities_observed.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data->patched_shaders.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data->scaled_textures_count.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data->scaled_viewports.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data->srv_blocked_corrections.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data->executed_command_lists.load(std::memory_order_relaxed)));
    {
        std::lock_guard<std::mutex> lock(data->summary_mutex);
        Log(kLogInfo,
            "[SPatch-ShadowScale] Viewport corrections per map class:");
        for (const auto& entry : data->viewport_corrections) {
            Log(kLogInfo,
                "[SPatch-ShadowScale]   map %ux%u -> %ux%u: %llu corrected viewports.",
                static_cast<unsigned>(entry.first.first),
                static_cast<unsigned>(entry.first.first),
                static_cast<unsigned>(entry.first.second),
                static_cast<unsigned>(entry.first.second),
                static_cast<unsigned long long>(entry.second));
        }
    }
    {
        std::lock_guard<std::mutex> lock(data->census_mutex);
        Log(kLogInfo,
            "[SPatch-ShadowScale] Shadow render passes (PS binds scaled map as DSV):");
        LogCensusTop(kLogInfo, data->render_counts, limit);
        Log(kLogInfo,
            "[SPatch-ShadowScale] Shadow consumers (PS samples scaled map as SRV):");
        LogCensusTop(kLogInfo, data->census_counts, limit);
    }
}

}  // namespace

namespace spatch::shadow_scale {

// Device capture point: called exactly once when SmaaRuntime publishes the
// game's D3D11 device (after the first successful Present). Replaces the
// ReShade init_device event.
// D3D11CreateDevice takes 10 arguments; D3D11CreateDeviceAndSwapChain takes
// 12 (pSwapChainDesc and ppSwapChain sit between SDKVersion and the device
// outputs). They MUST NOT share one forwarding signature: a 10-arg detour on
// the 12-arg export leaves the last two out-pointers (feature level,
// immediate context) forwarded from uninitialized stack slots, and d3d11
// writes the context through that garbage (crash dump 2026-10-31: AV write
// at d3d11+0x7A216 into d3d11's own code page; only survived while a local
// app-dir d3d11 proxy made LoadLibraryW return that copy instead of the
// system module).
using D3D11CreateDeviceFn = HRESULT(STDAPICALLTYPE*)(
    IDXGIAdapter*,
    D3D_DRIVER_TYPE,
    HMODULE,
    UINT,
    const D3D_FEATURE_LEVEL*,
    UINT,
    UINT,
    ID3D11Device**,
    D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);

using D3D11CreateDeviceAndSwapChainFn = HRESULT(STDAPICALLTYPE*)(
    IDXGIAdapter*,
    D3D_DRIVER_TYPE,
    HMODULE,
    UINT,
    const D3D_FEATURE_LEVEL*,
    UINT,
    UINT,
    const DXGI_SWAP_CHAIN_DESC*,
    IDXGISwapChain**,
    ID3D11Device**,
    D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);

void* g_create_device_original = nullptr;
void* g_create_device_swapchain_original = nullptr;
std::atomic<bool> g_create_device_hooked{false};

HRESULT STDAPICALLTYPE DetourD3D11CreateDevice(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    ID3D11Device** device_out,
    D3D_FEATURE_LEVEL* feature_level_out,
    ID3D11DeviceContext** immediate_context_out) {
    const auto original = reinterpret_cast<D3D11CreateDeviceFn>(
        g_create_device_original);
    if (!original) {
        return E_FAIL;
    }
    const HRESULT result =
        original(adapter, driver_type, software, flags, feature_levels,
                 feature_levels_count, sdk_version, device_out,
                 feature_level_out, immediate_context_out);
    if (SUCCEEDED(result) && device_out && *device_out) {
        NoteCapturedDevice(*device_out);
    }
    return result;
}

HRESULT STDAPICALLTYPE DetourD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels,
    UINT feature_levels_count,
    UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC* swap_chain_desc,
    IDXGISwapChain** swap_chain_out,
    ID3D11Device** device_out,
    D3D_FEATURE_LEVEL* feature_level_out,
    ID3D11DeviceContext** immediate_context_out) {
    const auto original =
        reinterpret_cast<D3D11CreateDeviceAndSwapChainFn>(
            g_create_device_swapchain_original);
    if (!original) {
        return E_FAIL;
    }
    const HRESULT result = original(
        adapter, driver_type, software, flags, feature_levels,
        feature_levels_count, sdk_version, swap_chain_desc, swap_chain_out,
        device_out, feature_level_out, immediate_context_out);
    if (SUCCEEDED(result) && device_out && *device_out) {
        NoteCapturedDevice(*device_out);
    }
    if (SUCCEEDED(result) && swap_chain_out && *swap_chain_out) {
        // Earliest possible SMAA DXGI install: the entry snapshot is captured
        // before the game's first present, ahead of the bundled Galaxy SDK's
        // present patcher. Later foreign patches are repaired per frame from
        // the owned snapshot (smaa::MaybeRepairOwnedHookEntries).
        smaa::MaybeInstallHooksForSwapChain(*swap_chain_out);
    }
    return result;
}

void StartDeviceWatchImpl() {
    // Force d3d11.dll to load NOW (app-dir local copy wins the search order,
    // exactly what the game would get) and hook its creation exports on this
    // thread. Doing this synchronously at bootstrap removes the race with the
    // game's first device creation; the game's later calls land on the hook.
    // Loading d3d11 early is harmless: the module is refcounted and the game
    // reuses the same one.
    HMODULE d3d11 = ::LoadLibraryW(L"d3d11.dll");
    if (d3d11 == nullptr) {
        Log(kLogInfo,
            "[SPatch-ShadowScale] d3d11.dll not loadable at bootstrap; shadow scaling will arm via the Present-capture path only.");
        return;
    }
    const auto create =
        reinterpret_cast<void*>(::GetProcAddress(d3d11, "D3D11CreateDevice"));
    const auto create_sc = reinterpret_cast<void*>(
        ::GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (create == nullptr && create_sc == nullptr) {
        Log(kLogWarning,
            "[SPatch-ShadowScale] d3d11 creation exports not found; shadow scaling will arm via the Present-capture path only.");
        return;
    }
    const MH_STATUS status_one =
        create != nullptr
            ? MH_CreateHook(create,
                            reinterpret_cast<void*>(&DetourD3D11CreateDevice),
                            &g_create_device_original)
            : MH_OK;
    const MH_STATUS status_two =
        create_sc != nullptr
            ? MH_CreateHook(
                  create_sc,
                  reinterpret_cast<void*>(&DetourD3D11CreateDeviceAndSwapChain),
                  &g_create_device_swapchain_original)
            : MH_OK;
    if (status_one == MH_OK && status_two == MH_OK) {
        Log(kLogInfo,
            "[SPatch-ShadowScale] d3d11 device-creation hooks armed (module=%p).",
            static_cast<void*>(d3d11));
    } else {
        Log(kLogWarning,
            "[SPatch-ShadowScale] d3d11 creation hook install failed (MH1=%d MH2=%d); shadow scaling disabled for this session.",
            static_cast<int>(status_one), static_cast<int>(status_two));
    }
}

void StartDeviceWatch() {
    try {
        StartDeviceWatchImpl();
    } catch (...) {
        // Bootstrap-path diagnostics must never take the game down.
    }
}

void NoteCapturedDevice(ID3D11Device* device) {
    if (device == nullptr) {
        return;
    }
    // Arms exactly once, fail-closed: a second captured device (or a lost
    // and recreated one) never re-arms. The MinHook hooks and trampolines
    // are process-lifetime, like every other SPatch detour.
    static std::atomic<bool> arm_attempted{false};
    if (arm_attempted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    try {
        if (g_module == nullptr) {
            ::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&NoteCapturedDevice), &g_module);
        }
        g_data.settings = LoadSettings();
        if (!g_data.settings.enabled) {
            return;
        }
        InstallNativeHooks(device);
    } catch (...) {
        g_data.settings.enabled = false;
        g_data.settings.texture_scale = 0.0f;
        g_data.settings.atlas_size = 0;
        Log(kLogError,
            "[SPatch-ShadowScale] Device initialization failed; retaining native shadow maps and filters.");
    }
}

// Frame cadence (replaces the ReShade present event): the game exits without
// releasing the device, so a frame-based snapshot (every ~15 s at 60 fps)
// guarantees the census reaches the log before the process goes away. A
// no-op unless the census debug key is set.
void NoteFrame() {
    DeviceData* const data = g_device_data;
    if (data == nullptr || !data->settings.census_consumers) {
        return;
    }
    constexpr std::uint32_t kSnapshotInterval = 900;  // ~15 s at 60 fps
    const std::uint32_t frame =
        data->present_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame % kSnapshotInterval == 0) {
        try {
            LogCensusSnapshot(data);
        } catch (...) {
            // Periodic census delivery is diagnostic only.
        }
    }
}

// Device teardown (replaces the ReShade destroy_device event). MinHook
// trampolines and the pinned ASI image remain valid for process lifetime;
// closing the gate makes every retained detour transparent.
void NoteDeviceLost() {
    DeviceData* const data = g_device_data;
    if (data != nullptr && data->settings.census_consumers) {
        try {
            LogCensusSnapshot(data, 20);
        } catch (...) {
            // Destruction and native-hook draining remain mandatory even if
            // the optional final census cannot allocate.
        }
    }
    if (data != nullptr) {
        (void)DeactivateNativeHooks();
    }
}

bool Enabled() {
    DeviceData* const data = g_device_data;
    return data != nullptr && data->settings.enabled;
}

}  // namespace spatch::shadow_scale
