#pragma once

#include <cstdint>
#include <string>
struct _SYSTEMTIME;

namespace Utils
{
std::wstring FormatRate(std::uint64_t bytes_per_sec);
std::wstring FormatBytes(std::uint64_t bytes);
std::wstring FormatPercent(std::uint64_t part, std::uint64_t whole);
std::wstring PaddedNumber(std::uint64_t value, unsigned width);
std::wstring FormatDate(const _SYSTEMTIME& time);
std::wstring FormatDateTime(const _SYSTEMTIME& time, bool seconds = false, wchar_t separator = L' ');
std::wstring ShortLabel(const std::wstring& exe_name, bool is_download);
std::wstring GetHistoryDirectory();
bool IsProtectedHistoryPath(const std::wstring& base_dir, const std::wstring& output_path);
}
