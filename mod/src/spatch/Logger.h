#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace spatch::log {

bool Initialize(const std::filesystem::path& path, bool enabled = true);
void Shutdown();

void Info(std::string_view message);
void Warn(std::string_view message);
void Error(std::string_view message);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void InfoF(const char* format, ...);
void WarnF(const char* format, ...);
void ErrorF(const char* format, ...);

std::string ToUtf8(std::wstring_view text);

}  // namespace spatch::log
