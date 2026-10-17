#include "Utils.h"

#include <Windows.h>
#include <filesystem>
#include <intrin.h>
#include <iterator>
#include <stdexcept>

namespace
{
std::wstring FormatValue(std::uint64_t value, bool per_sec)
{
    unsigned unit{};
    int digits{};
    std::uint64_t divisor = 1, scale = 1;
    constexpr const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB", L"EB" };
    while (unit + 1 < std::size(units) && value / divisor >= 1024) { divisor *= 1024; ++unit; }
    if (unit) { digits = unit == 1 ? 1 : 2; scale = digits == 1 ? 10 : 100; }
    auto integer = value / divisor;
    std::uint64_t high{}, remainder{};
    const auto numerator = _umul128(value % divisor, scale, &high);
    auto fraction = _udiv128(high, numerator, divisor, &remainder);
    // Match the existing nearest-even formatting without converting large byte
    // counters to double or linking the CRT floating-point formatter.
    if (remainder > divisor - remainder || (remainder == divisor - remainder && (fraction & 1))) ++fraction;
    if (fraction == scale) { ++integer; fraction = 0; }
    auto text = std::to_wstring(integer);
    if (digits) text += L"." + Utils::PaddedNumber(fraction, digits);
    text += L" ";
    text += units[unit];
    if (per_sec) text += L"/s";
    return text;
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

std::wstring Utils::FormatPercent(std::uint64_t part, std::uint64_t whole)
{
    std::uint64_t value{}, high{}, remainder{};
    if (whole)
    {
        const auto low = _umul128(part, 10000, &high);
        if (high >= whole) return L"n/a";
        value = _udiv128(high, low, whole, &remainder);
        if (remainder > whole - remainder || (remainder == whole - remainder && (value & 1)))
        {
            if (value == UINT64_MAX) return L"n/a";
            ++value;
        }
    }
    return std::to_wstring(value / 100) + L"." + PaddedNumber(value % 100, 2) + L"%";
}

std::wstring Utils::PaddedNumber(std::uint64_t value, unsigned width)
{
    auto text = std::to_wstring(value);
    if (text.size() < width) text.insert(0, width - text.size(), L'0');
    return text;
}

std::wstring Utils::FormatDate(const SYSTEMTIME& time)
{
    return PaddedNumber(time.wYear, 4) + L"-" + PaddedNumber(time.wMonth, 2) + L"-" + PaddedNumber(time.wDay, 2);
}

std::wstring Utils::FormatDateTime(const SYSTEMTIME& time, bool seconds, wchar_t separator)
{
    auto text = FormatDate(time);
    text.reserve(seconds ? 19 : 16);
    text += separator;
    text += PaddedNumber(time.wHour, 2) + L":" + PaddedNumber(time.wMinute, 2);
    if (seconds) text += L":" + PaddedNumber(time.wSecond, 2);
    return text;
}


std::wstring Utils::ShortLabel(const std::wstring& exe_name, bool is_download)
{
    std::wstring label = TrimExe(exe_name);
    label += is_download ? L"下" : L"上";
    return label;
}

std::wstring Utils::GetHistoryDirectory()
{
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable)) throw std::runtime_error("Cannot locate host executable");
    return (std::filesystem::path(executable).parent_path() / L"history").wstring();
}

bool Utils::IsProtectedHistoryPath(const std::wstring& base_dir, const std::wstring& output_path)
{
    if (base_dir.empty() || output_path.empty()) return true;
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(std::filesystem::path(base_dir), error).wstring();
    if (error) return true;
    const auto target = std::filesystem::weakly_canonical(std::filesystem::path(output_path), error).wstring();
    if (error) return true;
    // Keep the host's own statistics and the former plugin paths protected too.
    const auto base = std::filesystem::path(root);
    if (_wcsicmp(base.filename().c_str(), L"history") == 0 &&
        IsProtectedHistoryPath(base.parent_path().wstring(), output_path)) return true;
    if (CompareStringOrdinal(root.c_str(), -1, target.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
    auto prefix = root;
    if (prefix.back() != L'\\') prefix += L'\\';
    if (target.size() < prefix.size() || CompareStringOrdinal(target.c_str(), static_cast<int>(prefix.size()),
            prefix.c_str(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) return false;
    const auto relative = target.substr(prefix.size());
    const auto first = relative.substr(0, relative.find(L'\\'));
    return _wcsicmp(first.c_str(), L"app_traffic_history") == 0 || _wcsicmp(first.c_str(), L"network_traffic_history") == 0 ||
        _wcsnicmp(first.c_str(), L"app_traffic_state.tsv", 21) == 0 || _wcsnicmp(first.c_str(), L"history_traffic.dat", 19) == 0;
}

