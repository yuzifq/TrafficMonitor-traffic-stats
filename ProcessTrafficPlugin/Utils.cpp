#include "Utils.h"

#include <cstdio>

namespace
{
std::wstring FormatValue(std::uint64_t value, bool per_sec)
{
    wchar_t buffer[64]{};
    const double number = static_cast<double>(value);

    if (number < 1024.0)
    {
        swprintf_s(buffer, per_sec ? L"%.0f B/s" : L"%.0f B", number);
    }
    else if (number < 1024.0 * 1024.0)
    {
        swprintf_s(buffer, per_sec ? L"%.1f KB/s" : L"%.1f KB", number / 1024.0);
    }
    else if (number < 1024.0 * 1024.0 * 1024.0)
    {
        swprintf_s(buffer, per_sec ? L"%.2f MB/s" : L"%.2f MB", number / 1024.0 / 1024.0);
    }
    else
    {
        swprintf_s(buffer, per_sec ? L"%.2f GB/s" : L"%.2f GB", number / 1024.0 / 1024.0 / 1024.0);
    }

    return buffer;
}


std::wstring TrimExe(const std::wstring& exe_name)
{
    std::wstring result = exe_name;
    const auto dot = result.rfind(L'.');
    if (dot != std::wstring::npos)
    {
        result.erase(dot);
    }

    if (result.size() > 8)
    {
        result.resize(8);
    }

    return result;
}
}

std::wstring Utils::FormatRate(std::uint64_t bytes_per_sec)
{
    return FormatValue(bytes_per_sec, true);
}

std::wstring Utils::FormatBytes(std::uint64_t bytes)
{
    return FormatValue(bytes, false);
}


std::wstring Utils::JoinPids(const std::wstring& exe_name, const std::size_t pid_count)
{
    wchar_t buffer[128]{};
    swprintf_s(buffer, L"Target: %ls (%zu instance%ls)", exe_name.c_str(), pid_count, pid_count == 1 ? L"" : L"s");
    return buffer;
}

std::wstring Utils::ShortLabel(const std::wstring& exe_name, bool is_download)
{
    std::wstring label = TrimExe(exe_name);
    label += is_download ? L"下" : L"上";
    return label;
}

