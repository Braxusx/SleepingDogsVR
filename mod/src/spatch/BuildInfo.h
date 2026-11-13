#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace spatch::build_info {

inline constexpr std::wstring_view kSupportedVersionText = L"1.0.000.0";
inline constexpr std::uint16_t kSupportedVersionMajor = 1;
inline constexpr std::uint16_t kSupportedVersionMinor = 0;
inline constexpr std::uint16_t kSupportedVersionBuild = 0;
inline constexpr std::uint16_t kSupportedVersionPrivate = 0;
inline constexpr std::size_t kKnownFileSize = 37490688;
inline constexpr std::uint32_t kKnownSizeOfImage = 0x02765000;

inline constexpr std::uint32_t kLegacyTimeDateStamp = 0x5408D5E9;
inline constexpr std::array<std::uint8_t, 32> kLegacySha256 = {
    0xC6, 0xDB, 0x19, 0x9B, 0x76, 0x92, 0xD2, 0x42,
    0x31, 0xC2, 0x16, 0xFC, 0x29, 0xDC, 0x43, 0x0E,
    0xC3, 0xAF, 0xD5, 0x94, 0x35, 0xAD, 0x5C, 0x1A,
    0xC5, 0x89, 0x93, 0x4B, 0xE8, 0xCC, 0x60, 0x35};

inline constexpr std::uint32_t kLatestSteamTimeDateStamp = 0x543D6BB0;
inline constexpr std::array<std::uint8_t, 32> kLatestSteamSha256 = {
    0x2A, 0x33, 0xEC, 0x78, 0x7A, 0xC6, 0xFD, 0x4C,
    0x86, 0xFE, 0xC2, 0xB6, 0xF7, 0x78, 0xFE, 0xEA,
    0x88, 0x1A, 0x3F, 0x35, 0xEA, 0x56, 0xC6, 0x80,
    0x12, 0x1F, 0x53, 0x57, 0x1C, 0x05, 0x27, 0xDA};

// GoG release of the Definitive Edition.  Verified 2026-10-30 against the
// installed copy: same file size, size of image, and time-date stamp as the
// latest-Steam build, and a full byte diff of exactly 39 bytes limited to the
// steam_api64->common64 import name, "Data\"->"Save\" save-path strings, and
// five Steam-online branch patches.  No SPatch latest-layout RVA site lies
// within 80 bytes of any differing byte, so this build
// is classified with the latest-Steam hook layout and every site address
// applies unchanged.
inline constexpr std::array<std::uint8_t, 32> kGogSha256 = {
    0x49, 0x71, 0xC7, 0x57, 0x8D, 0xFA, 0x99, 0x6E,
    0xDB, 0x6D, 0x5E, 0xCE, 0xAE, 0xF1, 0xC0, 0xDB,
    0x59, 0xA1, 0xFA, 0xFE, 0xA5, 0x67, 0x32, 0x57,
    0x90, 0xFF, 0x05, 0x73, 0x7B, 0xD7, 0xC9, 0x77};

}  // namespace spatch::build_info
