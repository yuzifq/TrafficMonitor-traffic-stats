#pragma once

#include <cstdint>
#include <string>

namespace Utils
{
std::wstring FormatRate(std::uint64_t bytes_per_sec);
std::wstring FormatBytes(std::uint64_t bytes);
std::wstring JoinPids(const std::wstring& exe_name, const std::size_t pid_count);
std::wstring ShortLabel(const std::wstring& exe_name, bool is_download);
}
