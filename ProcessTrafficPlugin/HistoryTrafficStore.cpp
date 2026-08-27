#include "HistoryTrafficStore.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
using ExportBucketMap = std::map<std::wstring, std::map<std::wstring, CHistoryTrafficStore::TrafficAmount>>;

enum class SummaryKind
{
    Day,
    Week,
    Month,
    Quarter,
};

struct SummaryData
{
    std::map<std::uint64_t, CHistoryTrafficStore::TrafficAmount> byId;
    CHistoryTrafficStore::TrafficAmount total{};
    std::uint64_t sourceCount{};
    std::uint64_t sourceGeneration{};
};

constexpr std::uint64_t kFileTimePerDay = 864000000000ULL;

ULONGLONG SystemTimeValue(const SYSTEMTIME& time);

CHistoryTrafficStore::TrafficAmount MakeAmount(std::uint64_t rx, std::uint64_t tx)
{
    CHistoryTrafficStore::TrafficAmount amount{};
    amount.rxBytes = rx;
    amount.txBytes = tx;
    return amount;
}

void AddAmount(CHistoryTrafficStore::TrafficAmount& target, const CHistoryTrafficStore::TrafficAmount& delta)
{
    target.rxBytes += delta.rxBytes;
    target.txBytes += delta.txBytes;
}

bool IsSameAmount(
    const CHistoryTrafficStore::TrafficAmount& left,
    const CHistoryTrafficStore::TrafficAmount& right)
{
    return left.rxBytes == right.rxBytes && left.txBytes == right.txBytes;
}

bool TryParseUInt64(std::wstring_view text, std::uint64_t& value)
{
    if (text.empty())
    {
        return false;
    }

    std::uint64_t parsed = 0;
    for (wchar_t ch : text)
    {
        if (ch < L'0' || ch > L'9')
        {
            return false;
        }

        const auto digit = static_cast<std::uint64_t>(ch - L'0');
        if (parsed > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10)
        {
            return false;
        }

        parsed = parsed * 10 + digit;
    }

    value = parsed;
    return true;
}

constexpr std::uint64_t kInvalidAppId = (std::numeric_limits<std::uint64_t>::max)();
constexpr wchar_t kBase62Digits[] = L"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

std::wstring FormatStoredAppId(std::uint64_t value)
{
    std::wstring result;
    do
    {
        result.push_back(kBase62Digits[value % 62]);
        value /= 62;
    }
    while (value != 0);
    std::reverse(result.begin(), result.end());
    return result;
}

bool TryParseStoredAppId(std::wstring_view text, std::uint64_t& app_id)
{
    if (text.empty())
    {
        return false;
    }

    std::uint64_t parsed = 0;
    for (const auto ch : text)
    {
        const auto digit = std::wstring_view(kBase62Digits).find(ch);
        if (digit == std::wstring_view::npos ||
            parsed > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 62)
        {
            return false;
        }
        parsed = parsed * 62 + static_cast<std::uint64_t>(digit);
    }
    if (FormatStoredAppId(parsed) != text)
    {
        return false;
    }
    app_id = parsed;
    return true;
}

bool TryReadDailyHistoryLine(
    const std::wstring& date_key,
    const std::wstring& line,
    const std::unordered_map<std::uint64_t, std::wstring>& app_name_by_id,
    std::wstring& current_time_key,
    std::wstring& bucket_key,
    std::wstring& app_name,
    CHistoryTrafficStore::TrafficAmount& amount)
{
    const auto first_tab = line.find(L'\t');
    if (first_tab == std::wstring::npos)
    {
        return false;
    }

    const auto second_tab = line.find(L'\t', first_tab + 1);
    if (second_tab == std::wstring::npos)
    {
        return false;
    }

    const auto third_tab = line.find(L'\t', second_tab + 1);
    if (third_tab == std::wstring::npos)
    {
        return false;
    }

    std::uint64_t rx = 0;
    std::uint64_t tx = 0;
    const std::wstring_view rx_text(line.data() + second_tab + 1, third_tab - second_tab - 1);
    const std::wstring_view tx_text(line.data() + third_tab + 1, line.size() - third_tab - 1);
    if (!TryParseStoredAppId(rx_text, rx) || !TryParseStoredAppId(tx_text, tx))
    {
        return false;
    }

    const std::wstring_view stored_time_key(line.data(), first_tab);
    if (stored_time_key.size() == 5)
    {
        current_time_key.assign(stored_time_key);
    }
    else if (!stored_time_key.empty() || current_time_key.empty())
    {
        return false;
    }

    std::uint64_t app_id = 0;
    const std::wstring_view app_id_text(line.data() + first_tab + 1, second_tab - first_tab - 1);
    if (!TryParseStoredAppId(app_id_text, app_id))
    {
        return false;
    }
    const auto app_it = app_name_by_id.find(app_id);
    if (app_it == app_name_by_id.end())
    {
        return false;
    }

    bucket_key = date_key;
    bucket_key += L" ";
    bucket_key += current_time_key;
    app_name = app_it->second;
    amount = MakeAmount(rx, tx);
    return !bucket_key.empty() && !app_name.empty();
}

bool ReadTabField(std::wistringstream& stream, std::wstring& value)
{
    return static_cast<bool>(std::getline(stream, value, L'\t'));
}

bool ReadStoredAmount(std::wistringstream& stream, CHistoryTrafficStore::TrafficAmount& amount)
{
    std::wstring rx_text;
    std::wstring tx_text;
    if (!ReadTabField(stream, rx_text) || !ReadTabField(stream, tx_text))
    {
        return false;
    }

    return TryParseStoredAppId(rx_text, amount.rxBytes) &&
        TryParseStoredAppId(tx_text, amount.txBytes);
}

bool ReadDecimalAmount(std::wistringstream& stream, CHistoryTrafficStore::TrafficAmount& amount)
{
    std::wstring rx_text;
    std::wstring tx_text;
    if (!ReadTabField(stream, rx_text) || !ReadTabField(stream, tx_text))
    {
        return false;
    }
    return TryParseUInt64(rx_text, amount.rxBytes) && TryParseUInt64(tx_text, amount.txBytes);
}

void WriteStateLine(std::wofstream& output, const wchar_t* key, const std::wstring& value)
{
    output << key << L'\t' << value << L'\n';
}

void WriteStateLine(std::wofstream& output, const wchar_t* key, int value)
{
    output << key << L'\t' << value << L'\n';
}

void WriteAllTimeEntry(std::wofstream& output, const CHistoryTrafficStore::TrafficAmount& amount)
{
    output << L"all_time\t" << amount.rxBytes << L'\t' << amount.txBytes << L'\n';
}

void WritePathEntry(std::wofstream& output, const std::wstring& app_name, const std::wstring& exe_path)
{
    output << L"path\t" << app_name << L'\t' << exe_path << L'\n';
}

bool WriteDailyHistoryEntry(
    std::wofstream& output,
    const std::wstring& time_key,
    std::uint64_t app_id,
    const CHistoryTrafficStore::TrafficAmount& amount)
{
    output << time_key << L'\t'
           << FormatStoredAppId(app_id) << L'\t'
           << FormatStoredAppId(amount.rxBytes) << L'\t'
           << FormatStoredAppId(amount.txBytes) << L'\n';
    return static_cast<bool>(output);
}

bool TryParseDateKey(const std::wstring& text, SYSTEMTIME& date);
std::filesystem::path DailyHistoryPath(const std::wstring& history_root, const SYSTEMTIME& date);

bool DailyHistoryEndsWithBlock(
    const std::wstring& history_directory,
    const std::wstring& bucket_key,
    const std::unordered_map<std::uint64_t, std::wstring>& app_name_by_id,
    const std::unordered_map<std::wstring, CHistoryTrafficStore::TrafficAmount>& expected)
{
    if (history_directory.empty() || bucket_key.size() != 16 || expected.empty())
    {
        return false;
    }
    const auto date_key = bucket_key.substr(0, 10);
    const auto time_key = bucket_key.substr(11, 5);
    SYSTEMTIME date{};
    if (!TryParseDateKey(date_key, date))
    {
        return false;
    }
    const auto path = DailyHistoryPath(history_directory, date);
    if (!std::filesystem::exists(path))
    {
        return false;
    }

    std::unordered_map<std::wstring, CHistoryTrafficStore::TrafficAmount> last_matching_block;
    std::unordered_map<std::wstring, CHistoryTrafficStore::TrafficAmount> current_block;
    std::wstring current_time;
    std::wifstream input(path);
    std::wstring line;
    while (std::getline(input, line))
    {
        const auto first_tab = line.find(L'\t');
        const auto second_tab = first_tab == std::wstring::npos ? std::wstring::npos : line.find(L'\t', first_tab + 1);
        const auto third_tab = second_tab == std::wstring::npos ? std::wstring::npos : line.find(L'\t', second_tab + 1);
        if (third_tab == std::wstring::npos)
        {
            continue;
        }
        const auto stored_time = line.substr(0, first_tab);
        if (!stored_time.empty())
        {
            if (current_time == time_key)
            {
                last_matching_block = current_block;
            }
            current_time = stored_time;
            current_block.clear();
        }
        if (current_time != time_key)
        {
            continue;
        }
        std::uint64_t rx = 0;
        std::uint64_t tx = 0;
        const std::wstring_view rx_text(line.data() + second_tab + 1, third_tab - second_tab - 1);
        const std::wstring_view tx_text(line.data() + third_tab + 1, line.size() - third_tab - 1);
        if (TryParseStoredAppId(rx_text, rx) && TryParseStoredAppId(tx_text, tx))
        {
            std::uint64_t app_id = 0;
            const std::wstring_view app_id_text(line.data() + first_tab + 1, second_tab - first_tab - 1);
            if (!TryParseStoredAppId(app_id_text, app_id))
            {
                continue;
            }
            const auto app_it = app_name_by_id.find(app_id);
            if (app_it != app_name_by_id.end())
            {
                AddAmount(current_block[app_it->second], MakeAmount(rx, tx));
            }
        }
    }
    if (current_time == time_key)
    {
        last_matching_block = current_block;
    }
    if (last_matching_block.size() != expected.size())
    {
        return false;
    }
    for (const auto& entry : expected)
    {
        const auto found = last_matching_block.find(entry.first);
        if (found == last_matching_block.end() || !IsSameAmount(found->second, entry.second))
        {
            return false;
        }
    }
    return true;
}

constexpr std::array<unsigned char, 4> kTmhMagic{ 'T', 'M', 'H', '2' };
constexpr std::uint16_t kTmhVersion = 2;
constexpr std::size_t kTmhHeaderSize = 18;
constexpr std::size_t kTmhDayIndexSize = 45;

struct TmhDayIndex
{
    std::uint8_t day{};
    std::uint64_t dataOffset{};
    std::uint64_t dataLength{};
    std::uint64_t recordCount{};
    std::uint64_t rxTotal{};
    std::uint64_t txTotal{};
    std::uint32_t crc32{};
};

struct TmhMetadata
{
    std::uint16_t year{};
    std::uint8_t month{};
    std::vector<std::wstring> appNames;
    std::vector<TmhDayIndex> days;
};

template<typename T>
bool ReadLittleEndian(std::istream& input, T& value)
{
    static_assert(std::is_unsigned_v<T>);
    value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index)
    {
        const auto ch = input.get();
        if (ch == std::char_traits<char>::eof())
        {
            return false;
        }
        value |= static_cast<T>(static_cast<unsigned char>(ch)) << (index * 8);
    }
    return true;
}

bool ReadVarUInt(std::istream& input, std::uint64_t& value)
{
    value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 7)
    {
        const auto ch = input.get();
        if (ch == std::char_traits<char>::eof())
        {
            return false;
        }
        const auto byte = static_cast<unsigned char>(ch);
        if (shift == 63 && (byte & 0xfe) != 0)
        {
            return false;
        }
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
        {
            return true;
        }
    }
    return false;
}

bool ReadVarUInt(
    const unsigned char*& current,
    const unsigned char* end,
    std::uint64_t& value)
{
    value = 0;
    for (unsigned int shift = 0; shift < 64 && current < end; shift += 7)
    {
        const auto byte = *current++;
        if (shift == 63 && (byte & 0xfe) != 0)
        {
            return false;
        }
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
        {
            return true;
        }
    }
    return false;
}

std::uint32_t CalculateCrc32(const unsigned char* data, std::size_t size)
{
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index)
    {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const auto length = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const auto length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring SanitizeTsvField(const std::wstring& value)
{
    std::wstring result;
    result.reserve(value.size());
    for (const auto ch : value)
    {
        result.push_back(ch == L'\t' || ch == L'\r' || ch == L'\n' ? L' ' : ch);
    }
    return result;
}

bool ReadUtf8Line(std::ifstream& input, std::wstring& line)
{
    std::string raw;
    if (!std::getline(input, raw))
    {
        return false;
    }
    if (!raw.empty() && raw.back() == '\r')
    {
        raw.pop_back();
    }
    line = Utf8ToWide(raw);
    if (!line.empty() && line.front() == L'\ufeff')
    {
        line.erase(line.begin());
    }
    return raw.empty() || !line.empty();
}

void WriteUtf8Line(std::ofstream& output, const std::wstring& line)
{
    const auto utf8 = WideToUtf8(line);
    output.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    output.put('\n');
}

void LogHistoryIoFailure(const wchar_t* operation, const std::wstring& path, DWORD error)
{
    std::wstring message = operation;
    message += L" failed for ";
    message += path;
    message += L" (error ";
    message += std::to_wstring(error);
    message += L")\n";
    OutputDebugStringW(message.c_str());
}

void WriteUtf8StateLine(std::ofstream& output, const wchar_t* key, const std::wstring& value)
{
    WriteUtf8Line(output, std::wstring(key) + L"\t" + value);
}

void WriteUtf8StateLine(std::ofstream& output, const wchar_t* key, int value)
{
    WriteUtf8StateLine(output, key, std::to_wstring(value));
}

void WriteUtf8AllTimeEntry(std::ofstream& output, const CHistoryTrafficStore::TrafficAmount& amount)
{
    WriteUtf8Line(output, L"all_time\t" + std::to_wstring(amount.rxBytes) + L"\t" + std::to_wstring(amount.txBytes));
}

void WriteUtf8PathEntry(std::ofstream& output, const std::wstring& app_name, const std::wstring& exe_path)
{
    WriteUtf8Line(output, L"path\t" + SanitizeTsvField(app_name) + L"\t" + SanitizeTsvField(exe_path));
}

bool ReadCommittedMinute(const std::wstring& path, std::wstring& minute)
{
    if (path.empty() || !std::filesystem::exists(path))
    {
        return false;
    }
    std::ifstream input{ std::filesystem::path(path), std::ios::binary };
    return ReadUtf8Line(input, minute);
}

std::wstring FormatDateKey(const SYSTEMTIME& time)
{
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%04u-%02u-%02u", time.wYear, time.wMonth, time.wDay);
    return buffer;
}

bool TryParseDateKey(const std::wstring& text, SYSTEMTIME& date)
{
    date = {};
    if (text.size() != 10 || swscanf_s(text.c_str(), L"%hu-%hu-%hu", &date.wYear, &date.wMonth, &date.wDay) != 3)
    {
        return false;
    }
    SYSTEMTIME normalized = date;
    normalized.wHour = 0;
    normalized.wMinute = 0;
    normalized.wSecond = 0;
    normalized.wMilliseconds = 0;
    FILETIME file_time{};
    if (SystemTimeToFileTime(&normalized, &file_time) == FALSE ||
        date.wMonth < 1 || date.wMonth > 12 || date.wDay < 1 || date.wDay > 31)
    {
        return false;
    }
    date = normalized;
    return true;
}

bool AddDays(SYSTEMTIME& date, int days)
{
    FILETIME file_time{};
    if (SystemTimeToFileTime(&date, &file_time) == FALSE)
    {
        return false;
    }
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    if (days > 0)
    {
        value.QuadPart += kFileTimePerDay * static_cast<std::uint64_t>(days);
    }
    else if (days < 0)
    {
        value.QuadPart -= kFileTimePerDay * static_cast<std::uint64_t>(-days);
    }
    file_time.dwLowDateTime = value.LowPart;
    file_time.dwHighDateTime = value.HighPart;
    return FileTimeToSystemTime(&file_time, &date) != FALSE;
}

int DaysInMonth(unsigned short year, unsigned short month)
{
    SYSTEMTIME first{};
    first.wYear = year;
    first.wMonth = month;
    first.wDay = 1;
    SYSTEMTIME next = first;
    next.wMonth = month == 12 ? 1 : static_cast<WORD>(month + 1);
    next.wYear = month == 12 ? static_cast<WORD>(year + 1) : year;
    if (!AddDays(next, -1))
    {
        return 0;
    }
    return next.wDay;
}

int DayOfWeek(const SYSTEMTIME& date)
{
    FILETIME file_time{};
    if (SystemTimeToFileTime(&date, &file_time) == FALSE)
    {
        return -1;
    }
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return static_cast<int>((value.QuadPart / kFileTimePerDay + 1) % 7);
}

bool IsSameDate(const SYSTEMTIME& left, const SYSTEMTIME& right)
{
    return left.wYear == right.wYear && left.wMonth == right.wMonth && left.wDay == right.wDay;
}

bool IsBeforeDate(const SYSTEMTIME& left, const SYSTEMTIME& right)
{
    return SystemTimeValue(left) < SystemTimeValue(right);
}

bool IsCurrentDate(const SYSTEMTIME& date)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return IsSameDate(date, now);
}

std::wstring SummaryKindText(SummaryKind kind)
{
    switch (kind)
    {
    case SummaryKind::Day:
        return L"day";
    case SummaryKind::Week:
        return L"week";
    case SummaryKind::Month:
        return L"month";
    case SummaryKind::Quarter:
        return L"quarter";
    }
    return {};
}

std::filesystem::path YearDirectory(const std::wstring& history_root, unsigned short year)
{
    return std::filesystem::path(history_root) / std::to_wstring(year);
}

std::filesystem::path MonthDirectory(const std::wstring& history_root, unsigned short year, unsigned short month)
{
    wchar_t month_text[3]{};
    swprintf_s(month_text, L"%02u", month);
    return YearDirectory(history_root, year) / month_text;
}

std::filesystem::path DailyHistoryPath(const std::wstring& history_root, const SYSTEMTIME& date)
{
    return MonthDirectory(history_root, date.wYear, date.wMonth) / (FormatDateKey(date) + L".tsv");
}

std::filesystem::path DaySummaryPath(const std::wstring& history_root, const SYSTEMTIME& date)
{
    return MonthDirectory(history_root, date.wYear, date.wMonth) / (FormatDateKey(date) + L".day.tsv");
}

std::filesystem::path MonthSummaryPath(const std::wstring& history_root, const SYSTEMTIME& date)
{
    wchar_t name[32]{};
    swprintf_s(name, L"%04u-%02u.month.tsv", date.wYear, date.wMonth);
    return MonthDirectory(history_root, date.wYear, date.wMonth) / name;
}

std::filesystem::path QuarterSummaryPath(const std::wstring& history_root, const SYSTEMTIME& date)
{
    wchar_t name[32]{};
    swprintf_s(name, L"%04u-Q%u.quarter.tsv", date.wYear, (date.wMonth - 1) / 3 + 1);
    return YearDirectory(history_root, date.wYear) / name;
}

std::filesystem::path WeekSummaryPath(const std::wstring& history_root, const SYSTEMTIME& monday)
{
    SYSTEMTIME thursday = monday;
    AddDays(thursday, 3);
    SYSTEMTIME january_fourth{};
    january_fourth.wYear = thursday.wYear;
    january_fourth.wMonth = 1;
    january_fourth.wDay = 4;
    SYSTEMTIME first_monday = january_fourth;
    AddDays(first_monday, -((DayOfWeek(january_fourth) + 6) % 7));
    const auto distance = SystemTimeValue(monday) - SystemTimeValue(first_monday);
    const auto week = static_cast<unsigned int>(distance / kFileTimePerDay / 7 + 1);
    wchar_t name[32]{};
    swprintf_s(name, L"%04u-W%02u.week.tsv", thursday.wYear, week);
    return MonthDirectory(history_root, monday.wYear, monday.wMonth) / name;
}

void GetIsoWeek(const SYSTEMTIME& date, SYSTEMTIME& monday, SYSTEMTIME& sunday)
{
    monday = date;
    const auto day = DayOfWeek(date);
    AddDays(monday, -((day + 6) % 7));
    sunday = monday;
    AddDays(sunday, 6);
}

void GetMonthRange(const SYSTEMTIME& date, SYSTEMTIME& start, SYSTEMTIME& end)
{
    start = date;
    start.wDay = 1;
    end = start;
    end.wDay = static_cast<WORD>(DaysInMonth(start.wYear, start.wMonth));
}

void GetQuarterRange(const SYSTEMTIME& date, SYSTEMTIME& start, SYSTEMTIME& end)
{
    start = date;
    start.wMonth = static_cast<WORD>(((date.wMonth - 1) / 3) * 3 + 1);
    start.wDay = 1;
    end = start;
    end.wMonth = static_cast<WORD>(start.wMonth + 2);
    end.wDay = static_cast<WORD>(DaysInMonth(end.wYear, end.wMonth));
}

std::uint64_t GetSourceGeneration(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    const auto stamp = std::filesystem::last_write_time(path, error).time_since_epoch().count();
    return static_cast<std::uint64_t>(size) ^ static_cast<std::uint64_t>(stamp);
}

bool TryReadSummaryFile(
    const std::filesystem::path& path,
    SummaryKind expected_kind,
    const SYSTEMTIME& expected_start,
    const SYSTEMTIME& expected_end,
    SummaryData& summary)
{
    summary = {};
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return false;
    }

    bool version_ok = false;
    bool type_ok = false;
    bool start_ok = false;
    bool end_ok = false;
    bool complete_ok = false;
    bool total_seen = false;
    std::uint64_t previous_id = 0;
    bool has_previous_id = false;
    std::wstring line;
    while (ReadUtf8Line(input, line))
    {
        std::wistringstream stream(line);
        std::vector<std::wstring> fields;
        std::wstring field;
        while (std::getline(stream, field, L'\t'))
        {
            fields.push_back(field);
        }
        if (fields.size() == 2 && fields[0] == L"version")
        {
            std::uint64_t value = 0;
            version_ok = TryParseUInt64(fields[1], value) && value == 1;
        }
        else if (fields.size() == 2 && fields[0] == L"type")
        {
            type_ok = fields[1] == SummaryKindText(expected_kind);
        }
        else if (fields.size() == 2 && fields[0] == L"start")
        {
            start_ok = fields[1] == FormatDateKey(expected_start);
        }
        else if (fields.size() == 2 && fields[0] == L"end")
        {
            end_ok = fields[1] == FormatDateKey(expected_end);
        }
        else if (fields.size() == 2 && fields[0] == L"complete")
        {
            complete_ok = fields[1] == L"1";
        }
        else if (fields.size() == 2 && fields[0] == L"source_count")
        {
            summary.sourceCount = 0;
            if (!TryParseUInt64(fields[1], summary.sourceCount))
            {
                return false;
            }
        }
        else if (fields.size() == 2 && fields[0] == L"source_generation")
        {
            summary.sourceGeneration = 0;
            if (!TryParseUInt64(fields[1], summary.sourceGeneration))
            {
                return false;
            }
        }
        else if (fields.size() == 4 && fields[0] == L"app")
        {
            std::uint64_t id = 0;
            CHistoryTrafficStore::TrafficAmount amount{};
            std::wistringstream amount_stream(fields[2] + L"\t" + fields[3]);
            if (!TryParseStoredAppId(fields[1], id) ||
                (has_previous_id && id <= previous_id) ||
                !ReadStoredAmount(amount_stream, amount))
            {
                return false;
            }
            summary.byId.emplace(id, amount);
            previous_id = id;
            has_previous_id = true;
            AddAmount(summary.total, amount);
        }
        else if (fields.size() == 3 && fields[0] == L"total")
        {
            std::wistringstream total_stream(fields[1] + L"\t" + fields[2]);
            if (!ReadStoredAmount(total_stream, summary.total))
            {
                return false;
            }
            total_seen = true;
        }
        else
        {
            return false;
        }
    }
    if (!input.eof() || !version_ok || !type_ok || !start_ok || !end_ok || !complete_ok ||
        !total_seen || summary.sourceCount == 0)
    {
        return false;
    }

            CHistoryTrafficStore::TrafficAmount calculated{};
    for (const auto& entry : summary.byId)
    {
        AddAmount(calculated, entry.second);
    }
    return IsSameAmount(calculated, summary.total);
}

bool WriteSummaryFileAtomic(
    const std::filesystem::path& path,
    SummaryKind kind,
    const SYSTEMTIME& start,
    const SYSTEMTIME& end,
    std::uint64_t source_count,
    std::uint64_t source_generation,
    const SummaryData& summary)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto temporary = path.wstring() + L".new";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        return false;
    }
    WriteUtf8Line(output, L"version\t1");
    WriteUtf8Line(output, L"type\t" + SummaryKindText(kind));
    WriteUtf8Line(output, L"start\t" + FormatDateKey(start));
    WriteUtf8Line(output, L"end\t" + FormatDateKey(end));
    WriteUtf8Line(output, L"complete\t1");
    WriteUtf8Line(output, L"source_count\t" + std::to_wstring(source_count));
    WriteUtf8Line(output, L"source_generation\t" + std::to_wstring(source_generation));
    for (const auto& entry : summary.byId)
    {
        WriteUtf8Line(output, L"app\t" + FormatStoredAppId(entry.first) + L"\t" +
            FormatStoredAppId(entry.second.rxBytes) + L"\t" + FormatStoredAppId(entry.second.txBytes));
    }
    WriteUtf8Line(output, L"total\t" + FormatStoredAppId(summary.total.rxBytes) + L"\t" +
        FormatStoredAppId(summary.total.txBytes));
    output.flush();
    output.close();
    if (!output || MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

ULONGLONG SystemTimeValue(const SYSTEMTIME& time)
{
    FILETIME file_time{};
    if (SystemTimeToFileTime(&time, &file_time) == FALSE)
    {
        return 0;
    }
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return value.QuadPart;
}

bool TryReadTmhMetadata(const std::filesystem::path& path, TmhMetadata& metadata)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return false;
    }

    std::array<unsigned char, 4> magic{};
    input.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    std::uint16_t version = 0;
    std::uint8_t flags = 0;
    std::uint32_t app_count = 0;
    std::uint32_t day_count = 0;
    if (!input || magic != kTmhMagic ||
        !ReadLittleEndian(input, version) || version != kTmhVersion ||
        !ReadLittleEndian(input, metadata.year) ||
        !ReadLittleEndian(input, metadata.month) ||
        !ReadLittleEndian(input, flags) ||
        !ReadLittleEndian(input, app_count) ||
        !ReadLittleEndian(input, day_count) ||
        metadata.month < 1 || metadata.month > 12 ||
        app_count > 1000000 || day_count > 31)
    {
        return false;
    }

    metadata.appNames.clear();
    metadata.appNames.reserve(app_count);
    for (std::uint32_t index = 0; index < app_count; ++index)
    {
        std::uint64_t length = 0;
        if (!ReadVarUInt(input, length) || length == 0 || length > 32768)
        {
            return false;
        }
        std::string utf8_name(static_cast<std::size_t>(length), '\0');
        input.read(utf8_name.data(), static_cast<std::streamsize>(utf8_name.size()));
        auto app_name = Utf8ToWide(utf8_name);
        if (!input || app_name.empty())
        {
            return false;
        }
        metadata.appNames.push_back(std::move(app_name));
    }

    std::error_code error;
    const auto file_size = std::filesystem::file_size(path, error);
    if (error)
    {
        return false;
    }

    metadata.days.clear();
    metadata.days.reserve(day_count);
    std::set<std::uint8_t> seen_days;
    for (std::uint32_t index = 0; index < day_count; ++index)
    {
        TmhDayIndex day{};
        if (!ReadLittleEndian(input, day.day) ||
            !ReadLittleEndian(input, day.dataOffset) ||
            !ReadLittleEndian(input, day.dataLength) ||
            !ReadLittleEndian(input, day.recordCount) ||
            !ReadLittleEndian(input, day.rxTotal) ||
            !ReadLittleEndian(input, day.txTotal) ||
            !ReadLittleEndian(input, day.crc32) ||
            day.day < 1 || day.day > 31 ||
            !seen_days.insert(day.day).second ||
            day.dataOffset > file_size || day.dataLength > file_size - day.dataOffset)
        {
            return false;
        }
        metadata.days.push_back(day);
    }
    return true;
}

bool ReadTmhRange(
    const std::filesystem::path& path,
    const SYSTEMTIME& range_start_time,
    const SYSTEMTIME& range_end_time,
    std::unordered_map<std::wstring, CHistoryTrafficStore::TrafficAmount>* totals_by_app,
    ExportBucketMap* records_by_minute)
{
    TmhMetadata metadata{};
    if (!TryReadTmhMetadata(path, metadata))
    {
        return false;
    }

    const auto range_start = SystemTimeValue(range_start_time);
    const auto range_end = SystemTimeValue(range_end_time);
    std::unordered_map<std::wstring, CHistoryTrafficStore::TrafficAmount> archive_totals;
    ExportBucketMap archive_records;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return false;
    }

    for (const auto& day : metadata.days)
    {
        SYSTEMTIME day_start{};
        day_start.wYear = metadata.year;
        day_start.wMonth = metadata.month;
        day_start.wDay = day.day;
        SYSTEMTIME day_end = day_start;
        day_end.wHour = 23;
        day_end.wMinute = 59;
        if (SystemTimeValue(day_end) < range_start || SystemTimeValue(day_start) > range_end)
        {
            continue;
        }

        std::vector<unsigned char> data(static_cast<std::size_t>(day.dataLength));
        input.clear();
        input.seekg(static_cast<std::streamoff>(day.dataOffset), std::ios::beg);
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!input || CalculateCrc32(data.data(), data.size()) != day.crc32)
        {
            return false;
        }

        const auto* current = data.data();
        const auto* end = current + data.size();
        std::uint64_t minute_count = 0;
        if (!ReadVarUInt(current, end, minute_count) || minute_count > 1440)
        {
            return false;
        }

        std::uint64_t minute = 0;
        std::uint64_t parsed_records = 0;
        std::uint64_t parsed_rx = 0;
        std::uint64_t parsed_tx = 0;
        for (std::uint64_t minute_index = 0; minute_index < minute_count; ++minute_index)
        {
            std::uint64_t minute_delta = 0;
            std::uint64_t record_count = 0;
            if (!ReadVarUInt(current, end, minute_delta) ||
                !ReadVarUInt(current, end, record_count) ||
                record_count > metadata.appNames.size())
            {
                return false;
            }
            minute = minute_index == 0 ? minute_delta : minute + minute_delta;
            if (minute >= 1440)
            {
                return false;
            }

            SYSTEMTIME minute_time = day_start;
            minute_time.wHour = static_cast<WORD>(minute / 60);
            minute_time.wMinute = static_cast<WORD>(minute % 60);
            const auto minute_value = SystemTimeValue(minute_time);
            const bool selected = minute_value >= range_start && minute_value <= range_end;
            for (std::uint64_t record_index = 0; record_index < record_count; ++record_index)
            {
                std::uint64_t app_id = 0;
                std::uint64_t rx = 0;
                std::uint64_t tx = 0;
                if (!ReadVarUInt(current, end, app_id) ||
                    !ReadVarUInt(current, end, rx) ||
                    !ReadVarUInt(current, end, tx) ||
                    app_id >= metadata.appNames.size())
                {
                    return false;
                }
                if (selected)
                {
                    const auto& app_name = metadata.appNames[static_cast<std::size_t>(app_id)];
                    const auto amount = MakeAmount(rx, tx);
                    if (totals_by_app != nullptr)
                    {
                        AddAmount(archive_totals[app_name], amount);
                    }
                    if (records_by_minute != nullptr)
                    {
                        wchar_t bucket_key[32]{};
                        swprintf_s(
                            bucket_key,
                            L"%04u-%02u-%02u %02u:%02u",
                            minute_time.wYear,
                            minute_time.wMonth,
                            minute_time.wDay,
                            minute_time.wHour,
                            minute_time.wMinute);
                        AddAmount(archive_records[bucket_key][app_name], amount);
                    }
                }
                parsed_rx += rx;
                parsed_tx += tx;
                ++parsed_records;
            }
        }
        if (current != end || parsed_records != day.recordCount ||
            parsed_rx != day.rxTotal || parsed_tx != day.txTotal)
        {
            return false;
        }
    }

    if (totals_by_app != nullptr)
    {
        for (const auto& entry : archive_totals)
        {
            AddAmount((*totals_by_app)[entry.first], entry.second);
        }
    }
    if (records_by_minute != nullptr)
    {
        for (const auto& bucket_entry : archive_records)
        {
            for (const auto& app_entry : bucket_entry.second)
            {
                AddAmount((*records_by_minute)[bucket_entry.first][app_entry.first], app_entry.second);
            }
        }
    }
    return true;
}

bool AddTmhRangeTotals(
    const std::filesystem::path& path,
    const SYSTEMTIME& range_start_time,
    const SYSTEMTIME& range_end_time,
    std::unordered_map<std::wstring, CHistoryTrafficStore::TrafficAmount>& totals_by_app)
{
    return ReadTmhRange(path, range_start_time, range_end_time, &totals_by_app, nullptr);
}

using TmhRecordCallback = std::function<bool(
    const std::wstring& bucket_key,
    const std::wstring& app_name,
    const CHistoryTrafficStore::TrafficAmount& amount)>;

bool StreamTmhRange(
    const std::filesystem::path& path,
    const SYSTEMTIME& range_start_time,
    const SYSTEMTIME& range_end_time,
    const TmhRecordCallback& callback)
{
    TmhMetadata metadata{};
    if (!TryReadTmhMetadata(path, metadata))
    {
        return false;
    }

    const auto range_start = SystemTimeValue(range_start_time);
    const auto range_end = SystemTimeValue(range_end_time);
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return false;
    }

    for (const auto& day : metadata.days)
    {
        SYSTEMTIME day_start{};
        day_start.wYear = metadata.year;
        day_start.wMonth = metadata.month;
        day_start.wDay = day.day;
        SYSTEMTIME day_end = day_start;
        day_end.wHour = 23;
        day_end.wMinute = 59;
        if (SystemTimeValue(day_end) < range_start || SystemTimeValue(day_start) > range_end)
        {
            continue;
        }

        std::vector<unsigned char> data(static_cast<std::size_t>(day.dataLength));
        input.clear();
        input.seekg(static_cast<std::streamoff>(day.dataOffset), std::ios::beg);
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!input || CalculateCrc32(data.data(), data.size()) != day.crc32)
        {
            return false;
        }

        const auto* current = data.data();
        const auto* end = current + data.size();
        std::uint64_t minute_count = 0;
        if (!ReadVarUInt(current, end, minute_count) || minute_count > 1440)
        {
            return false;
        }

        std::uint64_t minute = 0;
        std::uint64_t parsed_records = 0;
        std::uint64_t parsed_rx = 0;
        std::uint64_t parsed_tx = 0;
        for (std::uint64_t minute_index = 0; minute_index < minute_count; ++minute_index)
        {
            std::uint64_t minute_delta = 0;
            std::uint64_t record_count = 0;
            if (!ReadVarUInt(current, end, minute_delta) ||
                !ReadVarUInt(current, end, record_count) ||
                record_count > metadata.appNames.size())
            {
                return false;
            }
            minute = minute_index == 0 ? minute_delta : minute + minute_delta;
            if (minute >= 1440)
            {
                return false;
            }

            SYSTEMTIME minute_time = day_start;
            minute_time.wHour = static_cast<WORD>(minute / 60);
            minute_time.wMinute = static_cast<WORD>(minute % 60);
            const auto minute_value = SystemTimeValue(minute_time);
            const bool selected = minute_value >= range_start && minute_value <= range_end;
            for (std::uint64_t record_index = 0; record_index < record_count; ++record_index)
            {
                std::uint64_t app_id = 0;
                std::uint64_t rx = 0;
                std::uint64_t tx = 0;
                if (!ReadVarUInt(current, end, app_id) ||
                    !ReadVarUInt(current, end, rx) ||
                    !ReadVarUInt(current, end, tx) ||
                    app_id >= metadata.appNames.size())
                {
                    return false;
                }
                if (selected)
                {
                    wchar_t bucket[32]{};
                    swprintf_s(
                        bucket,
                        L"%04u-%02u-%02u %02u:%02u",
                        minute_time.wYear,
                        minute_time.wMonth,
                        minute_time.wDay,
                        minute_time.wHour,
                        minute_time.wMinute);
                    if (!callback(bucket, metadata.appNames[static_cast<std::size_t>(app_id)], MakeAmount(rx, tx)))
                    {
                        return false;
                    }
                }
                parsed_rx += rx;
                parsed_tx += tx;
                ++parsed_records;
            }
        }
        if (current != end || parsed_records != day.recordCount ||
            parsed_rx != day.rxTotal || parsed_tx != day.txTotal)
        {
            return false;
        }
    }
    return true;
}

bool AddTmhRangeRecords(
    const std::filesystem::path& path,
    const SYSTEMTIME& range_start_time,
    const SYSTEMTIME& range_end_time,
    ExportBucketMap& records_by_minute)
{
    return ReadTmhRange(path, range_start_time, range_end_time, nullptr, &records_by_minute);
}

}

void CHistoryTrafficStore::Initialize(const std::wstring& base_dir)
{
    if (base_dir.empty())
    {
        return;
    }

    const auto normalized_base_dir = std::filesystem::path(base_dir).wstring();
    if (!m_baseDir.empty() && m_baseDir == normalized_base_dir && !m_historyDir.empty() && !m_stateFilePath.empty())
    {
        return;
    }

    m_baseDir = normalized_base_dir;
    std::filesystem::create_directories(std::filesystem::path(m_baseDir));
    m_historyDir = (std::filesystem::path(m_baseDir) / L"app_traffic_history").wstring();
    m_appDictionaryPath = (std::filesystem::path(m_historyDir) / L"apps.tsv").wstring();
    m_stateFilePath = (std::filesystem::path(m_baseDir) / L"app_traffic_state.tsv").wstring();
    m_checkpointFilePath = (std::filesystem::path(m_historyDir) / L"current.tmp").wstring();
    m_checkpointCommitPath = (std::filesystem::path(m_historyDir) / L"current.commit").wstring();
    m_loaded = false;
    m_bucketByApp.clear();
    m_bucketTimeRangeByKey.clear();
    m_currentMinuteKey.clear();
    m_currentMinuteTotals.clear();
    m_currentMinuteAppendUncertain = false;
    m_lastCheckpointTick = 0;
    m_appIdByName.clear();
    m_appNameById.clear();
    m_nextAppId = 1;
    m_pathByApp.clear();
    m_historyMigrationFormat = 0;
    m_hasPersistedAllTime = false;
    InvalidateCaches();
}

void CHistoryTrafficStore::Update(const std::vector<AppTotalEntry>& apps)
{
    EnsureLoaded();
    const auto bucket_key = GetCurrentMinuteKey();
    if (!m_currentMinuteKey.empty() && m_currentMinuteKey != bucket_key && !FlushCurrentMinute())
    {
        return;
    }
    if (m_currentMinuteKey.empty())
    {
        m_currentMinuteKey = bucket_key;
        m_currentMinuteAppendUncertain = false;
    }
    if (!EnsureBucketTimeRange(bucket_key))
    {
        return;
    }

    auto& bucket = m_bucketByApp[bucket_key];
    TrafficAmount total_delta{};
    bool history_dirty = false;
    bool state_dirty = false;
    bool path_dirty = false;

    for (const auto& app : apps)
    {
        const auto app_name = SanitizeTsvField(app.appName);
        const auto exe_path = SanitizeTsvField(app.exePath);
        if (app_name.empty())
        {
            continue;
        }
        const auto delta = MakeAmount(app.rxTotalBytes, app.txTotalBytes);

        if (delta.rxBytes != 0 || delta.txBytes != 0)
        {
            auto& stored = bucket[app_name];
            AddAmount(stored, delta);
            AddAmount(m_currentMinuteTotals[app_name], delta);
            AddAmount(total_delta, delta);
            history_dirty = true;
        }

        if (!exe_path.empty())
        {
            auto path_it = m_pathByApp.find(app_name);
            if (path_it == m_pathByApp.end())
            {
                m_pathByApp.emplace(app_name, exe_path);
                state_dirty = true;
                path_dirty = true;
            }
            else if (path_it->second != exe_path)
            {
                path_it->second = exe_path;
                state_dirty = true;
                path_dirty = true;
            }
        }

    }

    if (history_dirty)
    {
        InvalidateRangeCache();
        if (m_allTimeCacheValid)
        {
            AddAmount(m_cachedAllTimeTotal, total_delta);
        }
    }
    else if (path_dirty)
    {
        InvalidateRangeCache();
    }

    const auto now = GetTickCount64();
    const bool checkpoint_due = history_dirty &&
        (state_dirty || m_lastCheckpointTick == 0 || now - m_lastCheckpointTick >= 10000);
    if (checkpoint_due && !SaveCheckpoint())
    {
        LogHistoryIoFailure(L"SaveCheckpoint", m_checkpointFilePath, GetLastError());
    }
    if ((state_dirty || checkpoint_due) && !SaveState())
    {
        LogHistoryIoFailure(L"SaveState", m_stateFilePath, GetLastError());
    }
}

std::vector<CHistoryTrafficStore::AppTotalEntry> CHistoryTrafficStore::GetRangeAppTotals(const DateTimeRange& range) const
{
    const_cast<CHistoryTrafficStore*>(this)->EnsureLoaded();
    const auto normalized = NormalizeRange(range);
    if (m_rangeCacheValid && IsSameRange(m_cachedRange, normalized))
    {
        return m_cachedRangeApps;
    }

    std::unordered_map<std::wstring, TrafficAmount> totals_by_app;
    SYSTEMTIME today{};
    GetLocalTime(&today);
    SYSTEMTIME cursor = normalized.start;
    SYSTEMTIME query_end_date = normalized.end;
    cursor.wHour = 0;
    cursor.wMinute = 0;
    cursor.wSecond = 0;
    cursor.wMilliseconds = 0;
    query_end_date.wHour = 0;
    query_end_date.wMinute = 0;
    query_end_date.wSecond = 0;
    query_end_date.wMilliseconds = 0;

    const auto add_current_day = [&]() {
        const auto range_start = ToFileTimeValue(normalized.start);
        const auto range_end = ToFileTimeValue(normalized.end);
        for (const auto& bucket_entry : m_bucketByApp)
        {
            if (!EnsureBucketTimeRange(bucket_entry.first))
            {
                continue;
            }
            const auto range_it = m_bucketTimeRangeByKey.find(bucket_entry.first);
            if (range_it == m_bucketTimeRangeByKey.end() ||
                !BucketIntersectsRange(range_it->second, range_start, range_end))
            {
                continue;
            }
            for (const auto& app_entry : bucket_entry.second)
            {
                AddAmount(totals_by_app[app_entry.first], app_entry.second);
            }
        }
    };

    const auto add_summary = [&](const SummaryData& summary) {
        for (const auto& entry : summary.byId)
        {
            const auto app_it = m_appNameById.find(entry.first);
            if (app_it == m_appNameById.end())
            {
                return false;
            }
        }
        for (const auto& entry : summary.byId)
        {
            const auto app_it = m_appNameById.find(entry.first);
            AddAmount(totals_by_app[app_it->second], entry.second);
        }
        return true;
    };

    while (ToFileTimeValue(cursor) <= ToFileTimeValue(query_end_date))
    {
        if (IsCurrentDate(cursor))
        {
            add_current_day();
            break;
        }

        bool consumed = false;
        SYSTEMTIME period_start{};
        SYSTEMTIME period_end{};
        SummaryData summary{};
        const bool partial_day =
            (IsSameDate(cursor, normalized.start) &&
                (normalized.start.wHour != 0 || normalized.start.wMinute != 0)) ||
            (IsSameDate(cursor, normalized.end) &&
                (normalized.end.wHour != 23 || normalized.end.wMinute != 59));
        const auto try_summary = [&](SummaryKind kind, const SYSTEMTIME& start, const SYSTEMTIME& end, const std::filesystem::path& path) {
            if (partial_day || ToFileTimeValue(start) < ToFileTimeValue(cursor) ||
                ToFileTimeValue(end) > ToFileTimeValue(query_end_date) ||
                !IsBeforeDate(end, today))
            {
                return false;
            }
            SummaryData candidate{};
            if (!TryReadSummaryFile(path, kind, start, end, candidate) || !add_summary(candidate))
            {
                return false;
            }
            summary = std::move(candidate);
            return true;
        };

        if (cursor.wDay == 1 && (cursor.wMonth == 1 || cursor.wMonth == 4 || cursor.wMonth == 7 || cursor.wMonth == 10))
        {
            GetQuarterRange(cursor, period_start, period_end);
            consumed = try_summary(
                SummaryKind::Quarter,
                period_start,
                period_end,
                QuarterSummaryPath(m_historyDir, period_start));
        }
        if (!consumed && cursor.wDay == 1)
        {
            GetMonthRange(cursor, period_start, period_end);
            consumed = try_summary(
                SummaryKind::Month,
                period_start,
                period_end,
                MonthSummaryPath(m_historyDir, period_start));
        }
        if (!consumed)
        {
            SYSTEMTIME monday{};
            SYSTEMTIME sunday{};
            GetIsoWeek(cursor, monday, sunday);
            if (IsSameDate(cursor, monday))
            {
                consumed = try_summary(
                    SummaryKind::Week,
                    monday,
                    sunday,
                    WeekSummaryPath(m_historyDir, monday));
                if (consumed)
                {
                    period_start = monday;
                    period_end = sunday;
                }
            }
        }
        if (!consumed)
        {
            period_start = cursor;
            period_end = cursor;
            SummaryData day_summary{};
            consumed = try_summary(
                SummaryKind::Day,
                period_start,
                period_end,
                DaySummaryPath(m_historyDir, period_start));
        }

        if (consumed)
        {
            AddDays(period_end, 1);
            cursor = period_end;
            continue;
        }

        DateTimeRange day_range = normalized;
        day_range.start = cursor;
        day_range.start.wHour = 0;
        day_range.start.wMinute = 0;
        day_range.start.wSecond = 0;
        day_range.start.wMilliseconds = 0;
        day_range.end = cursor;
        day_range.end.wHour = 23;
        day_range.end.wMinute = 59;
        day_range.end.wSecond = 0;
        day_range.end.wMilliseconds = 0;
        if (IsSameDate(cursor, normalized.start))
        {
            day_range.start = normalized.start;
        }
        if (IsSameDate(cursor, normalized.end))
        {
            day_range.end = normalized.end;
        }
        const auto day_path = DailyHistoryPath(m_historyDir, cursor).wstring();
        if (!AddMinuteFileTotals(day_path, day_range, totals_by_app))
        {
            const auto tmh_path = MonthDirectory(m_historyDir, cursor.wYear, cursor.wMonth) /
                (FormatDateKey(cursor).substr(0, 7) + L".tmh");
            if (std::filesystem::exists(tmh_path))
            {
                AddTmhRangeTotals(tmh_path, day_range.start, day_range.end, totals_by_app);
            }
        }
        AddDays(cursor, 1);
    }

    std::vector<AppTotalEntry> result;
    result.reserve(totals_by_app.size());
    TrafficAmount total{};
    for (const auto& entry : totals_by_app)
    {
        AppTotalEntry item{};
        item.appName = entry.first;
        const auto path_it = m_pathByApp.find(entry.first);
        if (path_it != m_pathByApp.end())
        {
            item.exePath = path_it->second;
        }
        item.rxTotalBytes = entry.second.rxBytes;
        item.txTotalBytes = entry.second.txBytes;
        AddAmount(total, entry.second);
        result.push_back(item);
    }
    m_cachedRange = normalized;
    m_cachedRangeApps = result;
    m_cachedRangeTotal = total;
    m_rangeCacheValid = true;
    return result;
}

CHistoryTrafficStore::TrafficAmount CHistoryTrafficStore::GetRangeTotal(const DateTimeRange& range) const
{
    const auto normalized = NormalizeRange(range);
    if (!m_rangeCacheValid || !IsSameRange(m_cachedRange, normalized))
    {
        static_cast<void>(GetRangeAppTotals(normalized));
    }
    return m_cachedRangeTotal;
}

CHistoryTrafficStore::TrafficAmount CHistoryTrafficStore::GetAllTimeTotal() const
{
    const_cast<CHistoryTrafficStore*>(this)->EnsureLoaded();
    if (m_allTimeCacheValid)
    {
        return m_cachedAllTimeTotal;
    }

    TrafficAmount total{};
    for (const auto& bucket_entry : m_bucketByApp)
    {
        for (const auto& app_entry : bucket_entry.second)
        {
            AddAmount(total, app_entry.second);
        }
    }
    m_cachedAllTimeTotal = total;
    m_allTimeCacheValid = true;
    return total;
}

bool CHistoryTrafficStore::ExportRange(
    const DateTimeRange& range,
    const std::wstring& output_path,
    std::wstring& error_message) const
{
    error_message.clear();
    if (output_path.empty())
    {
        error_message = L"No output file was selected.";
        return false;
    }

    const_cast<CHistoryTrafficStore*>(this)->EnsureLoaded();
    const auto normalized = NormalizeRange(range);
    const auto target = std::filesystem::path(output_path);
    const auto temporary = std::filesystem::path(output_path + L".tmp");
    std::error_code filesystem_error;
    if (!target.parent_path().empty())
    {
        std::filesystem::create_directories(target.parent_path(), filesystem_error);
    }
    std::filesystem::remove(temporary, filesystem_error);

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        error_message = L"The selected output file could not be opened.";
        return false;
    }

    const bool multiple_days = !IsSameDate(normalized.start, normalized.end);
    std::wstring current_date;
    std::wstring current_bucket;
    bool wrote_record = false;
    bool write_failed = false;
    const auto write_record = [&](const std::wstring& bucket_key,
                                  const std::wstring& app_name,
                                  const TrafficAmount& amount) {
        if (bucket_key.size() != 16 || app_name.empty())
        {
            return false;
        }
        const auto date_key = bucket_key.substr(0, 10);
        const auto time_key = bucket_key.substr(11, 5);
        if (multiple_days && date_key != current_date)
        {
            std::wstring date_line;
            if (!current_date.empty())
            {
                date_line += L'\n';
            }
            date_line += date_key + L"\n";
            const auto utf8_date_line = WideToUtf8(date_line);
            output.write(utf8_date_line.data(), static_cast<std::streamsize>(utf8_date_line.size()));
            current_date = date_key;
            current_bucket.clear();
        }
        if (bucket_key != current_bucket)
        {
            const auto utf8_header = WideToUtf8(time_key + L"\tUpload\tDownload\n");
            output.write(utf8_header.data(), static_cast<std::streamsize>(utf8_header.size()));
            current_bucket = bucket_key;
        }
        const auto utf8_line = WideToUtf8(
            app_name + L"\t" + std::to_wstring(amount.txBytes) + L"\t" + std::to_wstring(amount.rxBytes) + L"\n");
        output.write(utf8_line.data(), static_cast<std::streamsize>(utf8_line.size()));
        wrote_record = static_cast<bool>(output);
        return wrote_record;
    };

    SYSTEMTIME today{};
    GetLocalTime(&today);
    SYSTEMTIME cursor = normalized.start;
    SYSTEMTIME end_date = normalized.end;
    cursor.wHour = 0;
    cursor.wMinute = 0;
    cursor.wSecond = 0;
    cursor.wMilliseconds = 0;
    end_date.wHour = 0;
    end_date.wMinute = 0;
    end_date.wSecond = 0;
    end_date.wMilliseconds = 0;

    while (ToFileTimeValue(cursor) <= ToFileTimeValue(end_date))
    {
        DateTimeRange day_range = normalized;
        day_range.start = cursor;
        day_range.start.wHour = 0;
        day_range.start.wMinute = 0;
        day_range.start.wSecond = 0;
        day_range.start.wMilliseconds = 0;
        day_range.end = cursor;
        day_range.end.wHour = 23;
        day_range.end.wMinute = 59;
        day_range.end.wSecond = 0;
        day_range.end.wMilliseconds = 0;
        if (IsSameDate(cursor, normalized.start))
        {
            day_range.start = normalized.start;
        }
        if (IsSameDate(cursor, normalized.end))
        {
            day_range.end = normalized.end;
        }

        if (IsCurrentDate(cursor))
        {
            std::map<std::wstring, std::vector<std::pair<std::wstring, TrafficAmount>>> current_records;
            for (const auto& bucket_entry : m_bucketByApp)
            {
                if (!EnsureBucketTimeRange(bucket_entry.first))
                {
                    continue;
                }
                const auto range_it = m_bucketTimeRangeByKey.find(bucket_entry.first);
                if (range_it == m_bucketTimeRangeByKey.end() ||
                    !BucketIntersectsRange(range_it->second, ToFileTimeValue(day_range.start), ToFileTimeValue(day_range.end)))
                {
                    continue;
                }
                auto& records = current_records[bucket_entry.first];
                for (const auto& app_entry : bucket_entry.second)
                {
                    records.emplace_back(app_entry.first, app_entry.second);
                }
            }
            for (auto& bucket_entry : current_records)
            {
                std::sort(bucket_entry.second.begin(), bucket_entry.second.end(), [](const auto& left, const auto& right) {
                    return left.first < right.first;
                });
                for (const auto& app_entry : bucket_entry.second)
                {
                    if (!write_record(bucket_entry.first, app_entry.first, app_entry.second))
                    {
                        write_failed = true;
                        break;
                    }
                }
                if (write_failed)
                {
                    break;
                }
            }
        }
        else
        {
            const auto daily_path = DailyHistoryPath(m_historyDir, cursor);
            if (std::filesystem::exists(daily_path))
            {
                std::ifstream input(daily_path, std::ios::binary);
                if (!input.is_open())
                {
                    error_message = L"A daily history file could not be opened: " + daily_path.wstring();
                    write_failed = true;
                }
                else
                {
                    std::wstring line;
                    std::wstring current_time_key;
                    const auto start_value = ToFileTimeValue(day_range.start);
                    const auto end_value = ToFileTimeValue(day_range.end);
                    while (!write_failed && ReadUtf8Line(input, line))
                    {
                        std::wstring bucket_key;
                        std::wstring app_name;
                        TrafficAmount amount{};
                        if (!TryReadDailyHistoryLine(FormatDateKey(cursor), line, m_appNameById, current_time_key, bucket_key, app_name, amount))
                        {
                            error_message = L"A daily history file is invalid: " + daily_path.wstring();
                            write_failed = true;
                            break;
                        }
                        if (!EnsureBucketTimeRange(bucket_key))
                        {
                            continue;
                        }
                        const auto bucket_it = m_bucketTimeRangeByKey.find(bucket_key);
                        if (bucket_it != m_bucketTimeRangeByKey.end() &&
                            BucketIntersectsRange(bucket_it->second, start_value, end_value) &&
                            !write_record(bucket_key, app_name, amount))
                        {
                            write_failed = true;
                        }
                    }
                }
            }
            else
            {
                const auto tmh_path = MonthDirectory(m_historyDir, cursor.wYear, cursor.wMonth) /
                    (FormatDateKey(cursor).substr(0, 7) + L".tmh");
                if (std::filesystem::exists(tmh_path) &&
                    !StreamTmhRange(tmh_path, day_range.start, day_range.end,
                        [&](const std::wstring& bucket_key, const std::wstring& app_name, const TrafficAmount& amount) {
                            return write_record(bucket_key, app_name, amount);
                        }))
                {
                    error_message = L"An archived history file is invalid: " + tmh_path.wstring();
                    write_failed = true;
                }
            }
        }
        if (write_failed)
        {
            break;
        }
        if (!AddDays(cursor, 1))
        {
            break;
        }
    }
    if (write_failed)
    {
        std::filesystem::remove(temporary, filesystem_error);
        if (error_message.empty())
        {
            error_message = L"Traffic history could not be written to the selected file.";
        }
        return false;
    }
    if (!wrote_record)
    {
        output.close();
        std::filesystem::remove(temporary, filesystem_error);
        error_message = L"No traffic history exists in the selected range.";
        return false;
    }
    output.flush();
    output.close();
    if (!output)
    {
        std::filesystem::remove(temporary, filesystem_error);
        error_message = L"Traffic history could not be written to the selected file.";
        return false;
    }
    if (MoveFileExW(
            temporary.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        const auto system_error = GetLastError();
        std::filesystem::remove(temporary, filesystem_error);
        error_message = L"The exported file could not be finalized (Windows error " +
            std::to_wstring(system_error) + L").";
        return false;
    }
    return true;
}

CHistoryTrafficStore::DateTimeRange CHistoryTrafficStore::GetPreferredRange() const
{
    const_cast<CHistoryTrafficStore*>(this)->EnsureLoaded();
    return m_preferredRange;
}

void CHistoryTrafficStore::SetPreferredRange(const DateTimeRange& range)
{
    EnsureLoaded();
    const auto normalized = NormalizeRange(range);
    if (IsSameRange(m_preferredRange, normalized))
    {
        return;
    }

    m_preferredRange = normalized;
    SaveState();
}

CHistoryTrafficStore::DisplayLanguage CHistoryTrafficStore::GetPreferredLanguage() const
{
    const_cast<CHistoryTrafficStore*>(this)->EnsureLoaded();
    return m_preferredLanguage;
}

void CHistoryTrafficStore::SetPreferredLanguage(DisplayLanguage language)
{
    EnsureLoaded();
    if (m_preferredLanguage == language)
    {
        return;
    }

    m_preferredLanguage = language;
    SaveState();
}

void CHistoryTrafficStore::EnsureLoaded()
{
    if (m_loaded)
    {
        return;
    }

    if (m_baseDir.empty())
    {
        wchar_t module_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, module_path, MAX_PATH);
        m_baseDir = std::filesystem::path(module_path).parent_path().wstring();
        m_historyDir = (std::filesystem::path(m_baseDir) / L"app_traffic_history").wstring();
        m_appDictionaryPath = (std::filesystem::path(m_historyDir) / L"apps.tsv").wstring();
        m_stateFilePath = (std::filesystem::path(m_baseDir) / L"app_traffic_state.tsv").wstring();
        m_checkpointFilePath = (std::filesystem::path(m_historyDir) / L"current.tmp").wstring();
        m_checkpointCommitPath = (std::filesystem::path(m_historyDir) / L"current.commit").wstring();
    }

    LoadState();
    LoadAppDictionary();
    EnsureCompletedSummaries();
    Load();
    m_loaded = true;
}

bool CHistoryTrafficStore::EnsureCompletedSummaries()
{
    if (m_historyDir.empty() || !std::filesystem::exists(m_historyDir))
    {
        return true;
    }

    SYSTEMTIME today{};
    GetLocalTime(&today);
    today.wHour = 0;
    today.wMinute = 0;
    today.wSecond = 0;
    today.wMilliseconds = 0;

    std::vector<std::pair<std::filesystem::path, SYSTEMTIME>> daily_files;
    std::error_code error;
    for (const auto& year_entry : std::filesystem::directory_iterator(m_historyDir, error))
    {
        if (error || !year_entry.is_directory())
        {
            continue;
        }
        const auto year_name = year_entry.path().filename().wstring();
        std::uint64_t year_value = 0;
        if (!TryParseUInt64(year_name, year_value) || year_value < 1601 || year_value > 9999)
        {
            continue;
        }
        for (const auto& month_entry : std::filesystem::directory_iterator(year_entry.path(), error))
        {
            if (error || !month_entry.is_directory())
            {
                continue;
            }
            const auto month_name = month_entry.path().filename().wstring();
            std::uint64_t month_value = 0;
            if (!TryParseUInt64(month_name, month_value) || month_value < 1 || month_value > 12)
            {
                continue;
            }
            for (const auto& file_entry : std::filesystem::directory_iterator(month_entry.path(), error))
            {
                if (error || !file_entry.is_regular_file())
                {
                    continue;
                }
                if (file_entry.path().extension() != L".tsv")
                {
                    continue;
                }
                SYSTEMTIME date{};
                if (!TryParseDateKey(file_entry.path().stem().wstring(), date) || IsCurrentDate(date))
                {
                    continue;
                }
                daily_files.emplace_back(file_entry.path(), date);
            }
        }
    }

    const auto build_day_summary = [&](const std::filesystem::path& path, const SYSTEMTIME& date, SummaryData& summary, std::uint64_t& source_count) {
        summary = {};
        source_count = 0;
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            return false;
        }
        const auto date_key = FormatDateKey(date);
        std::wstring line;
        std::wstring current_time_key;
        while (ReadUtf8Line(input, line))
        {
            std::wstring bucket_key;
            std::wstring app_name;
            TrafficAmount amount{};
            if (!TryReadDailyHistoryLine(date_key, line, m_appNameById, current_time_key, bucket_key, app_name, amount))
            {
                return false;
            }
            const auto app_it = m_appIdByName.find(app_name);
            if (app_it == m_appIdByName.end())
            {
                return false;
            }
            AddAmount(summary.byId[app_it->second], amount);
            AddAmount(summary.total, amount);
            ++source_count;
        }
        if (!input.eof())
        {
            return false;
        }
        if (source_count == 0)
        {
            source_count = 1;
        }
        return true;
    };

    for (const auto& daily : daily_files)
    {
        SummaryData existing{};
        if (TryReadSummaryFile(DaySummaryPath(m_historyDir, daily.second), SummaryKind::Day, daily.second, daily.second, existing))
        {
            continue;
        }
        SummaryData summary{};
        std::uint64_t source_count = 0;
        if (build_day_summary(daily.first, daily.second, summary, source_count))
        {
            WriteSummaryFileAtomic(
                DaySummaryPath(m_historyDir, daily.second),
                SummaryKind::Day,
                daily.second,
                daily.second,
                source_count,
                GetSourceGeneration(daily.first),
                summary);
        }
    }

    const auto combine = [](SummaryData& target, const SummaryData& source) {
        for (const auto& entry : source.byId)
        {
            AddAmount(target.byId[entry.first], entry.second);
        }
        AddAmount(target.total, source.total);
        target.sourceGeneration ^= source.sourceGeneration + 0x9e3779b97f4a7c15ULL +
            (target.sourceGeneration << 6) + (target.sourceGeneration >> 2);
    };

    const auto read_day_summary = [&](const SYSTEMTIME& date, SummaryData& summary) {
        return TryReadSummaryFile(
            DaySummaryPath(m_historyDir, date),
            SummaryKind::Day,
            date,
            date,
            summary);
    };

    std::set<std::wstring> seen_weeks;
    std::set<std::wstring> seen_months;
    for (const auto& daily : daily_files)
    {
        SYSTEMTIME month_start{};
        SYSTEMTIME month_end{};
        GetMonthRange(daily.second, month_start, month_end);
        if (IsBeforeDate(month_end, today) && seen_months.insert(FormatDateKey(month_start).substr(0, 7)).second)
        {
            SummaryData month_summary{};
            bool complete = true;
            std::uint64_t source_count = 0;
            SYSTEMTIME date = month_start;
            while (ToFileTimeValue(date) <= ToFileTimeValue(month_end))
            {
                SummaryData day_summary{};
                if (!read_day_summary(date, day_summary))
                {
                    complete = false;
                    break;
                }
                combine(month_summary, day_summary);
                ++source_count;
                AddDays(date, 1);
            }
            const auto summary_path = MonthSummaryPath(m_historyDir, month_start);
            SummaryData existing{};
            if (TryReadSummaryFile(summary_path, SummaryKind::Month, month_start, month_end, existing))
            {
                continue;
            }
            if (complete)
            {
                WriteSummaryFileAtomic(
                    summary_path,
                    SummaryKind::Month,
                    month_start,
                    month_end,
                    source_count,
                    month_summary.sourceGeneration,
                    month_summary);
            }
        }

        SYSTEMTIME monday{};
        SYSTEMTIME sunday{};
        GetIsoWeek(daily.second, monday, sunday);
        const auto week_key = FormatDateKey(monday);
        if (IsBeforeDate(sunday, today) && seen_weeks.insert(week_key).second)
        {
            SummaryData week_summary{};
            bool complete = true;
            std::uint64_t source_count = 0;
            SYSTEMTIME date = monday;
            while (ToFileTimeValue(date) <= ToFileTimeValue(sunday))
            {
                SummaryData day_summary{};
                if (!read_day_summary(date, day_summary))
                {
                    complete = false;
                    break;
                }
                combine(week_summary, day_summary);
                ++source_count;
                AddDays(date, 1);
            }
            const auto summary_path = WeekSummaryPath(m_historyDir, monday);
            SummaryData existing{};
            if (TryReadSummaryFile(summary_path, SummaryKind::Week, monday, sunday, existing))
            {
                continue;
            }
            if (complete)
            {
                WriteSummaryFileAtomic(
                    summary_path,
                    SummaryKind::Week,
                    monday,
                    sunday,
                    source_count,
                    week_summary.sourceGeneration,
                    week_summary);
            }
        }
    }

    for (unsigned int year = 1601; year <= 9999; ++year)
    {
        for (unsigned short month = 1; month <= 12; month = static_cast<unsigned short>(month + 3))
        {
            SYSTEMTIME quarter_start{};
            quarter_start.wYear = static_cast<WORD>(year);
            quarter_start.wMonth = month;
            quarter_start.wDay = 1;
            SYSTEMTIME quarter_end{};
            const SYSTEMTIME quarter_date = quarter_start;
            GetQuarterRange(quarter_date, quarter_start, quarter_end);
            if (!IsBeforeDate(quarter_end, today))
            {
                continue;
            }
            SummaryData quarter_summary{};
            bool complete = true;
            std::uint64_t source_count = 0;
            for (unsigned short child_month = month; child_month <= month + 2; ++child_month)
            {
                SYSTEMTIME month_date{};
                month_date.wYear = static_cast<WORD>(year);
                month_date.wMonth = child_month;
                month_date.wDay = 1;
                SYSTEMTIME month_end{};
                GetMonthRange(month_date, month_date, month_end);
                SummaryData month_summary{};
                if (!TryReadSummaryFile(
                        MonthSummaryPath(m_historyDir, month_date),
                        SummaryKind::Month,
                        month_date,
                        month_end,
                        month_summary))
                {
                    complete = false;
                    break;
                }
                combine(quarter_summary, month_summary);
                ++source_count;
            }
            const auto summary_path = QuarterSummaryPath(m_historyDir, quarter_start);
            SummaryData existing{};
            if (TryReadSummaryFile(summary_path, SummaryKind::Quarter, quarter_start, quarter_end, existing))
            {
                continue;
            }
            if (complete)
            {
                WriteSummaryFileAtomic(
                    summary_path,
                    SummaryKind::Quarter,
                    quarter_start,
                    quarter_end,
                    source_count,
                    quarter_summary.sourceGeneration,
                    quarter_summary);
            }
        }
    }
    return true;
}

void CHistoryTrafficStore::Load()
{
    m_bucketByApp.clear();
    m_bucketTimeRangeByKey.clear();
    InvalidateRangeCache();
    TrafficAmount loaded_total{};
    SYSTEMTIME today{};
    GetLocalTime(&today);
    LoadHistoryFile(DailyHistoryPath(m_historyDir, today).wstring(), loaded_total);
    if (!m_hasPersistedAllTime)
    {
        m_cachedAllTimeTotal = loaded_total;
        m_allTimeCacheValid = true;
    }
    LoadCheckpoint();
}

bool CHistoryTrafficStore::LoadAppDictionary()
{
    m_appIdByName.clear();
    m_appNameById.clear();
    m_nextAppId = 1;
    if (m_appDictionaryPath.empty() || !std::filesystem::exists(m_appDictionaryPath))
    {
        return false;
    }

    std::ifstream input(std::filesystem::path(m_appDictionaryPath), std::ios::binary);
    bool has_current_version = false;
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }
        if (line == "version\t4")
        {
            has_current_version = true;
            continue;
        }
        if (line.rfind("version\t", 0) == 0)
        {
            return false;
        }
        const auto first_tab = line.find('\t');
        const auto second_tab = first_tab == std::string::npos ? std::string::npos : line.find('\t', first_tab + 1);
        if (first_tab == std::string::npos || second_tab == std::string::npos || line.substr(0, first_tab) != "app")
        {
            return false;
        }
        const auto id_text = line.substr(first_tab + 1, second_tab - first_tab - 1);
        std::uint64_t app_id = 0;
        const auto id_wide = Utf8ToWide(id_text);
        auto app_name = Utf8ToWide(line.substr(second_tab + 1));
        if (!TryParseStoredAppId(id_wide, app_id) || app_name.empty() ||
            m_appNameById.find(app_id) != m_appNameById.end() ||
            m_appIdByName.find(app_name) != m_appIdByName.end())
        {
            return false;
        }
        m_appIdByName.emplace(app_name, app_id);
        m_appNameById.emplace(app_id, std::move(app_name));
        if (app_id >= m_nextAppId)
        {
            m_nextAppId = app_id + 1;
        }
    }
    return has_current_version && (static_cast<bool>(input) || input.eof());
}

bool CHistoryTrafficStore::SaveAppDictionary() const
{
    if (m_appDictionaryPath.empty())
    {
        return false;
    }
    std::filesystem::create_directories(std::filesystem::path(m_historyDir));
    const auto temporary = std::filesystem::path(m_appDictionaryPath + L".new");
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        return false;
    }
    output << "version\t4\n";
    std::vector<std::pair<std::uint64_t, std::wstring>> entries;
    entries.reserve(m_appNameById.size());
    for (const auto& entry : m_appNameById)
    {
        entries.emplace_back(entry.first, entry.second);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    for (const auto& entry : entries)
    {
        const auto utf8_name = WideToUtf8(entry.second);
        if (utf8_name.empty())
        {
            return false;
        }
        output << "app\t" << WideToUtf8(FormatStoredAppId(entry.first)) << '\t' << utf8_name << '\n';
    }
    output.flush();
    output.close();
    if (!output || MoveFileExW(
            temporary.c_str(),
            m_appDictionaryPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

std::uint64_t CHistoryTrafficStore::EnsureAppId(const std::wstring& app_name)
{
    const auto found = m_appIdByName.find(app_name);
    if (found != m_appIdByName.end())
    {
        return found->second;
    }
    if (app_name.empty() || m_nextAppId == kInvalidAppId)
    {
        return kInvalidAppId;
    }
    const auto app_id = m_nextAppId++;
    m_appIdByName.emplace(app_name, app_id);
    m_appNameById.emplace(app_id, app_name);
    if (!SaveAppDictionary())
    {
        m_appIdByName.erase(app_name);
        m_appNameById.erase(app_id);
        m_nextAppId = app_id;
        return kInvalidAppId;
    }
    return app_id;
}

void CHistoryTrafficStore::LoadHistoryFile(const std::wstring& path, TrafficAmount& loaded_total)
{
    if (path.empty() || !std::filesystem::exists(path))
    {
        return;
    }

    const auto date_key = std::filesystem::path(path).stem().wstring();
    SYSTEMTIME day_start{};
    SYSTEMTIME day_end{};
    if (!TryParseBucketKey(date_key, day_start, day_end))
    {
        return;
    }

    std::ifstream input{ std::filesystem::path(path), std::ios::binary };
    std::wstring line;
    std::wstring current_time_key;
    while (ReadUtf8Line(input, line))
    {
        std::wstring bucket_key;
        std::wstring app_name;
        TrafficAmount amount{};
        if (!TryReadDailyHistoryLine(date_key, line, m_appNameById, current_time_key, bucket_key, app_name, amount) ||
            !EnsureBucketTimeRange(bucket_key))
        {
            continue;
        }

        auto& entry = m_bucketByApp[bucket_key][app_name];
        AddAmount(entry, amount);
        AddAmount(loaded_total, amount);
    }
}

void CHistoryTrafficStore::LoadHistoryDirectory(const std::wstring& directory, TrafficAmount& loaded_total)
{
    if (directory.empty() || !std::filesystem::exists(directory))
    {
        return;
    }

    for (const auto& file_entry : std::filesystem::directory_iterator(std::filesystem::path(directory)))
    {
        if (!file_entry.is_regular_file())
        {
            continue;
        }

        const auto path = file_entry.path();
        const auto date_key = path.stem().wstring();
        if (path.extension() != L".tsv" || date_key.size() != 10)
        {
            continue;
        }

        SYSTEMTIME day_start{};
        SYSTEMTIME day_end{};
        if (!TryParseBucketKey(date_key, day_start, day_end))
        {
            continue;
        }

        std::wifstream input{ path };
        std::wstring line;
        std::wstring current_time_key;
        while (std::getline(input, line))
        {
            std::wstring bucket_key;
            std::wstring app_name;
            TrafficAmount amount{};
            if (!TryReadDailyHistoryLine(date_key, line, m_appNameById, current_time_key, bucket_key, app_name, amount) ||
                !EnsureBucketTimeRange(bucket_key))
            {
                continue;
            }

            auto& entry = m_bucketByApp[bucket_key][app_name];
            AddAmount(entry, amount);
            AddAmount(loaded_total, amount);
        }
    }
}

bool CHistoryTrafficStore::AddMinuteFileTotals(
    const std::wstring& path,
    const DateTimeRange& range,
    std::unordered_map<std::wstring, TrafficAmount>& totals_by_app) const
{
    if (path.empty() || !std::filesystem::exists(path))
    {
        return false;
    }

    const auto date_key = std::filesystem::path(path).stem().wstring();
    SYSTEMTIME day_start{};
    SYSTEMTIME day_end{};
    if (!TryParseBucketKey(date_key, day_start, day_end))
    {
        return false;
    }

    const auto range_start = ToFileTimeValue(range.start);
    const auto range_end = ToFileTimeValue(range.end);
    std::ifstream input{ std::filesystem::path(path), std::ios::binary };
    if (!input.is_open())
    {
        return false;
    }

    std::wstring line;
    std::wstring current_time_key;
    while (ReadUtf8Line(input, line))
    {
        std::wstring bucket_key;
        std::wstring app_name;
        TrafficAmount amount{};
        if (!TryReadDailyHistoryLine(date_key, line, m_appNameById, current_time_key, bucket_key, app_name, amount))
        {
            return false;
        }
        if (!EnsureBucketTimeRange(bucket_key))
        {
            return false;
        }
        const auto bucket_range = m_bucketTimeRangeByKey.find(bucket_key);
        if (bucket_range == m_bucketTimeRangeByKey.end() ||
            !BucketIntersectsRange(bucket_range->second, range_start, range_end))
        {
            continue;
        }
        AddAmount(totals_by_app[app_name], amount);
    }
    return input.eof();
}

void CHistoryTrafficStore::AddRangeHistoryFromDirectory(
    const std::wstring& directory,
    const DateTimeRange& range,
    std::unordered_map<std::wstring, TrafficAmount>& totals_by_app) const
{
    const auto range_start = ToFileTimeValue(range.start);
    if (directory.empty() ||
        !std::filesystem::exists(directory))
    {
        return;
    }

    const auto range_end = ToFileTimeValue(range.end);
    for (const auto& file_entry : std::filesystem::directory_iterator(std::filesystem::path(directory)))
    {
        if (!file_entry.is_regular_file())
        {
            continue;
        }
        const auto path = file_entry.path();
        const auto month_key = path.stem().wstring();
        if (path.extension() != L".tmh" || month_key.size() != 7)
        {
            continue;
        }

        unsigned short year = 0;
        unsigned short month = 0;
        if (swscanf_s(month_key.c_str(), L"%hu-%hu", &year, &month) != 2 || month < 1 || month > 12)
        {
            continue;
        }
        SYSTEMTIME month_start{};
        month_start.wYear = year;
        month_start.wMonth = month;
        month_start.wDay = 1;
        SYSTEMTIME month_end = month_start;
        month_end.wMonth = month == 12 ? 1 : static_cast<WORD>(month + 1);
        month_end.wYear = month == 12 ? static_cast<WORD>(year + 1) : year;
        const auto next_month_value = ToFileTimeValue(month_end);
        const auto month_end_value = next_month_value >= 600000000ULL ? next_month_value - 600000000ULL : 0;
        bool archive_valid = true;
        if (month_end_value >= range_start && ToFileTimeValue(month_start) <= range_end)
        {
            archive_valid = AddTmhRangeTotals(path, range.start, range.end, totals_by_app);
        }
        else
        {
            TmhMetadata metadata{};
            archive_valid = TryReadTmhMetadata(path, metadata);
        }
        static_cast<void>(archive_valid);
    }
}

bool CHistoryTrafficStore::AppendHistoryEntries(
    const std::wstring& bucket_key,
    const std::vector<std::pair<std::wstring, TrafficAmount>>& entries)
{
    if (m_historyDir.empty() || entries.empty())
    {
        return false;
    }

    const auto date_key = GetDateKeyFromMinuteKey(bucket_key);
    const auto time_key = GetTimeKeyFromMinuteKey(bucket_key);
    if (date_key.empty() || time_key.empty())
    {
        return false;
    }

    SYSTEMTIME date{};
    if (!TryParseDateKey(date_key, date))
    {
        return false;
    }
    const auto file_path = DailyHistoryPath(m_historyDir, date);
    std::filesystem::create_directories(file_path.parent_path());
    std::wofstream output{ file_path, std::ios::app };
    if (!output.is_open())
    {
        return false;
    }

    bool first_entry = true;
    for (const auto& entry : entries)
    {
        const auto app_id = EnsureAppId(entry.first);
        if (app_id == kInvalidAppId ||
            !WriteDailyHistoryEntry(output, first_entry ? time_key : std::wstring{}, app_id, entry.second))
        {
            return false;
        }
        first_entry = false;
    }
    output.flush();
    return static_cast<bool>(output);
}

void CHistoryTrafficStore::LoadCheckpoint()
{
    m_currentMinuteKey.clear();
    m_currentMinuteTotals.clear();
    m_currentMinuteAppendUncertain = false;
    if (m_checkpointFilePath.empty() || !std::filesystem::exists(m_checkpointFilePath))
    {
        return;
    }

    std::wstring minute_key;
    TrafficAmount checkpoint_all_time{};
    bool has_all_time = false;
    BucketAppMap entries;
    std::ifstream input{ std::filesystem::path(m_checkpointFilePath), std::ios::binary };
    std::wstring line;
    while (ReadUtf8Line(input, line))
    {
        std::wistringstream stream(line);
        std::wstring type;
        if (!ReadTabField(stream, type))
        {
            continue;
        }
        if (type == L"minute")
        {
            ReadTabField(stream, minute_key);
        }
        else if (type == L"all_time")
        {
            has_all_time = ReadDecimalAmount(stream, checkpoint_all_time);
        }
        else if (type == L"entry")
        {
            std::wstring app_id_text;
            TrafficAmount amount{};
            std::uint64_t app_id = 0;
            if (ReadTabField(stream, app_id_text) &&
                TryParseStoredAppId(app_id_text, app_id) &&
                ReadStoredAmount(stream, amount))
            {
                const auto app_it = m_appNameById.find(app_id);
                if (app_it != m_appNameById.end())
                {
                    AddAmount(entries[app_it->second], amount);
                }
            }
        }
    }

    SYSTEMTIME minute_start{};
    SYSTEMTIME minute_end{};
    if (entries.empty() || !TryParseBucketKey(minute_key, minute_start, minute_end) || minute_key.size() != 16)
    {
        return;
    }

    if (has_all_time)
    {
        m_cachedAllTimeTotal.rxBytes = (std::max)(m_cachedAllTimeTotal.rxBytes, checkpoint_all_time.rxBytes);
        m_cachedAllTimeTotal.txBytes = (std::max)(m_cachedAllTimeTotal.txBytes, checkpoint_all_time.txBytes);
        m_allTimeCacheValid = true;
    }

    std::wstring committed_minute;
    if (!m_checkpointCommitPath.empty() && std::filesystem::exists(m_checkpointCommitPath))
    {
        ReadCommittedMinute(m_checkpointCommitPath, committed_minute);
    }
    if (committed_minute == minute_key ||
        DailyHistoryEndsWithBlock(m_historyDir, minute_key, m_appNameById, entries))
    {
        ClearCheckpoint();
        if (!SaveState())
        {
            LogHistoryIoFailure(L"SaveState", m_stateFilePath, GetLastError());
        }
        return;
    }

    m_currentMinuteKey = minute_key;
    m_currentMinuteTotals = entries;
    m_currentMinuteAppendUncertain = false;
    EnsureBucketTimeRange(minute_key);
    for (const auto& entry : entries)
    {
        AddAmount(m_bucketByApp[minute_key][entry.first], entry.second);
    }
    m_lastCheckpointTick = GetTickCount64();
}

bool CHistoryTrafficStore::SaveCheckpoint()
{
    if (m_checkpointFilePath.empty() || m_currentMinuteKey.empty() || m_currentMinuteTotals.empty())
    {
        return false;
    }

    std::vector<std::pair<std::uint64_t, TrafficAmount>> stored_entries;
    stored_entries.reserve(m_currentMinuteTotals.size());
    for (const auto& entry : m_currentMinuteTotals)
    {
        const auto app_id = EnsureAppId(entry.first);
        if (app_id == kInvalidAppId)
        {
            return false;
        }
        stored_entries.emplace_back(app_id, entry.second);
    }

    std::filesystem::create_directories(std::filesystem::path(m_historyDir));
    const auto temporary_path = m_checkpointFilePath + L".new";
    std::ofstream output{ std::filesystem::path(temporary_path), std::ios::binary | std::ios::trunc };
    if (!output.is_open())
    {
        return false;
    }
    WriteUtf8StateLine(output, L"version", 5);
    WriteUtf8StateLine(output, L"minute", m_currentMinuteKey);
    WriteUtf8AllTimeEntry(output, m_cachedAllTimeTotal);
    for (const auto& entry : stored_entries)
    {
        WriteUtf8Line(output, L"entry\t" + FormatStoredAppId(entry.first) + L'\t'
            + FormatStoredAppId(entry.second.rxBytes) + L'\t'
            + FormatStoredAppId(entry.second.txBytes));
    }
    output.flush();
    output.close();
    if (!output)
    {
        LogHistoryIoFailure(L"WriteCheckpoint", temporary_path, GetLastError());
        std::error_code error;
        std::filesystem::remove(temporary_path, error);
        return false;
    }
    if (MoveFileExW(
            temporary_path.c_str(),
            m_checkpointFilePath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        LogHistoryIoFailure(L"ReplaceCheckpoint", m_checkpointFilePath, GetLastError());
        std::error_code error;
        std::filesystem::remove(temporary_path, error);
        return false;
    }
    m_lastCheckpointTick = GetTickCount64();
    return true;
}

bool CHistoryTrafficStore::FlushCurrentMinute()
{
    if (m_currentMinuteKey.empty() || m_currentMinuteTotals.empty())
    {
        m_currentMinuteKey.clear();
        m_currentMinuteTotals.clear();
        m_currentMinuteAppendUncertain = false;
        ClearCheckpoint();
        return true;
    }

    if (!SaveCheckpoint())
    {
        return false;
    }

    std::vector<std::pair<std::wstring, TrafficAmount>> entries;
    entries.reserve(m_currentMinuteTotals.size());
    for (const auto& entry : m_currentMinuteTotals)
    {
        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    std::wstring committed_minute;
    const bool already_appended =
        (ReadCommittedMinute(m_checkpointCommitPath, committed_minute) &&
            committed_minute == m_currentMinuteKey) ||
        DailyHistoryEndsWithBlock(m_historyDir, m_currentMinuteKey, m_appNameById, m_currentMinuteTotals);
    if (!already_appended && !AppendHistoryEntries(m_currentMinuteKey, entries))
    {
        m_currentMinuteAppendUncertain = true;
        if (!SaveCheckpoint())
        {
            LogHistoryIoFailure(L"SaveCheckpoint", m_checkpointFilePath, GetLastError());
        }
        return false;
    }

    const auto commit_temporary = m_checkpointCommitPath + L".new";
    std::ofstream commit_output{ std::filesystem::path(commit_temporary), std::ios::binary | std::ios::trunc };
    WriteUtf8Line(commit_output, m_currentMinuteKey);
    commit_output.flush();
    commit_output.close();
    if (!commit_output)
    {
        LogHistoryIoFailure(L"WriteMinuteCommit", commit_temporary, GetLastError());
        std::error_code error;
        std::filesystem::remove(commit_temporary, error);
        return false;
    }
    if (MoveFileExW(
            commit_temporary.c_str(),
            m_checkpointCommitPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        LogHistoryIoFailure(L"ReplaceMinuteCommit", m_checkpointCommitPath, GetLastError());
        return false;
    }

    ClearCheckpoint();
    m_currentMinuteKey.clear();
    m_currentMinuteTotals.clear();
    m_currentMinuteAppendUncertain = false;
    m_lastCheckpointTick = 0;
    if (!SaveState())
    {
        LogHistoryIoFailure(L"SaveState", m_stateFilePath, GetLastError());
    }
    return true;
}

void CHistoryTrafficStore::ClearCheckpoint() const
{
    if (m_checkpointFilePath.empty())
    {
        return;
    }
    std::error_code error;
    std::filesystem::remove(std::filesystem::path(m_checkpointFilePath), error);
}

void CHistoryTrafficStore::LoadState()
{
    m_pathByApp.clear();
    InvalidateRangeCache();
    m_preferredRange = GetDefaultRange();
    m_preferredLanguage = DisplayLanguage::English;
    m_historyMigrationFormat = 0;
    m_hasPersistedAllTime = false;
    m_allTimeCacheValid = false;
    m_cachedAllTimeTotal = {};

    if (m_stateFilePath.empty() || !std::filesystem::exists(m_stateFilePath))
    {
        return;
    }

    std::ifstream input{ std::filesystem::path(m_stateFilePath), std::ios::binary };
    std::wstring line;
    while (ReadUtf8Line(input, line))
    {
        std::wistringstream stream(line);
        std::wstring type;
        if (!std::getline(stream, type, L'\t'))
        {
            continue;
        }

        if (type == L"language")
        {
            std::wstring value_text;
            if (ReadTabField(stream, value_text))
            {
                const int value = _wtoi(value_text.c_str());
                if (value >= static_cast<int>(DisplayLanguage::English) && value <= static_cast<int>(DisplayLanguage::Chinese))
                {
                    m_preferredLanguage = static_cast<DisplayLanguage>(value);
                }
            }
            continue;
        }

        if (type == L"history_migration_format")
        {
            std::wstring value_text;
            if (ReadTabField(stream, value_text))
            {
                m_historyMigrationFormat = (std::max)(0, _wtoi(value_text.c_str()));
            }
            continue;
        }

        if (type == L"range_start")
        {
            std::wstring value_text;
            if (ReadTabField(stream, value_text))
            {
                TryParseStoredTime(value_text, m_preferredRange.start);
            }
            continue;
        }

        if (type == L"range_end")
        {
            std::wstring value_text;
            if (ReadTabField(stream, value_text))
            {
                TryParseStoredTime(value_text, m_preferredRange.end);
            }
            continue;
        }

        if (type == L"path")
        {
            std::wstring app_name;
            std::wstring exe_path;
            if (ReadTabField(stream, app_name) && ReadTabField(stream, exe_path) && !app_name.empty() && !exe_path.empty())
            {
                m_pathByApp[app_name] = exe_path;
            }
            continue;
        }

        if (type == L"all_time")
        {
            TrafficAmount amount{};
            if (ReadDecimalAmount(stream, amount))
            {
                m_cachedAllTimeTotal = amount;
                m_allTimeCacheValid = true;
                m_hasPersistedAllTime = true;
            }
            continue;
        }

    }

    m_preferredRange = NormalizeRange(m_preferredRange);
}

bool CHistoryTrafficStore::SaveState() const
{
    if (m_stateFilePath.empty())
    {
        return false;
    }

    const auto normalized = NormalizeRange(m_preferredRange);
    const auto temporary_path = m_stateFilePath + L".new";
    std::ofstream output{ std::filesystem::path(temporary_path), std::ios::binary | std::ios::trunc };
    if (!output.is_open())
    {
        return false;
    }
    WriteUtf8StateLine(output, L"history_format", 8);
    WriteUtf8StateLine(output, L"history_archive_format", 2);
    if (m_historyMigrationFormat > 0)
    {
        WriteUtf8StateLine(output, L"history_migration_format", m_historyMigrationFormat);
    }
    WriteUtf8StateLine(output, L"language", static_cast<int>(m_preferredLanguage));
    WriteUtf8StateLine(output, L"range_start", FormatMinuteTime(normalized.start));
    WriteUtf8StateLine(output, L"range_end", FormatMinuteTime(normalized.end));
    if (m_allTimeCacheValid)
    {
        WriteUtf8AllTimeEntry(output, m_cachedAllTimeTotal);
    }
    for (const auto& entry : m_pathByApp)
    {
        if (!entry.first.empty() && !entry.second.empty())
        {
            WriteUtf8PathEntry(output, entry.first, entry.second);
        }
    }
    output.flush();
    output.close();
    if (!output)
    {
        LogHistoryIoFailure(L"WriteState", temporary_path, GetLastError());
        std::error_code error;
        std::filesystem::remove(temporary_path, error);
        return false;
    }
    if (MoveFileExW(
            temporary_path.c_str(),
            m_stateFilePath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
        LogHistoryIoFailure(L"ReplaceState", m_stateFilePath, GetLastError());
        std::error_code error;
        std::filesystem::remove(temporary_path, error);
        return false;
    }
    return true;
}

CHistoryTrafficStore::DateTimeRange CHistoryTrafficStore::GetDefaultRange()
{
    DateTimeRange range{};
    GetLocalTime(&range.end);
    range.start = range.end;
    range.start.wHour = 0;
    range.start.wMinute = 0;
    range.start.wSecond = 0;
    range.start.wMilliseconds = 0;
    range.end.wSecond = 0;
    range.end.wMilliseconds = 0;
    return range;
}

CHistoryTrafficStore::DateTimeRange CHistoryTrafficStore::NormalizeRange(const DateTimeRange& range)
{
    DateTimeRange normalized = range;
    NormalizeSystemTime(normalized.start);
    NormalizeSystemTime(normalized.end);

    if (ToFileTimeValue(normalized.start) > ToFileTimeValue(normalized.end))
    {
        normalized.end = normalized.start;
    }

    return normalized;
}

std::wstring CHistoryTrafficStore::GetCurrentMinuteKey()
{
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    return FormatMinuteTime(local_time);
}

std::wstring CHistoryTrafficStore::FormatMinuteTime(const SYSTEMTIME& time)
{
    wchar_t buffer[20]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute);
    return buffer;
}

std::wstring CHistoryTrafficStore::GetDateKeyFromMinuteKey(const std::wstring& bucket_key)
{
    if (bucket_key.size() < 10)
    {
        return {};
    }

    return bucket_key.substr(0, 10);
}

std::wstring CHistoryTrafficStore::GetTimeKeyFromMinuteKey(const std::wstring& bucket_key)
{
    if (bucket_key.size() < 16)
    {
        return {};
    }

    return bucket_key.substr(11, 5);
}

bool CHistoryTrafficStore::TryParseStoredTime(const std::wstring& text, SYSTEMTIME& time)
{
    SYSTEMTIME parsed{};
    if (swscanf_s(text.c_str(), L"%hu-%hu-%hu %hu:%hu",
        &parsed.wYear,
        &parsed.wMonth,
        &parsed.wDay,
        &parsed.wHour,
        &parsed.wMinute) != 5)
    {
        return false;
    }

    NormalizeSystemTime(parsed);
    time = parsed;
    return true;
}

bool CHistoryTrafficStore::TryParseBucketKey(const std::wstring& bucket_key, SYSTEMTIME& bucket_start, SYSTEMTIME& bucket_end)
{
    bucket_start = {};
    bucket_end = {};

    if (bucket_key.size() == 10)
    {
        if (swscanf_s(bucket_key.c_str(), L"%hu-%hu-%hu",
            &bucket_start.wYear,
            &bucket_start.wMonth,
            &bucket_start.wDay) != 3)
        {
            return false;
        }

        bucket_end = bucket_start;
        bucket_start.wHour = 0;
        bucket_start.wMinute = 0;
        bucket_end.wHour = 23;
        bucket_end.wMinute = 59;
    }
    else
    {
        if (swscanf_s(bucket_key.c_str(), L"%hu-%hu-%hu %hu:%hu",
            &bucket_start.wYear,
            &bucket_start.wMonth,
            &bucket_start.wDay,
            &bucket_start.wHour,
            &bucket_start.wMinute) != 5)
        {
            return false;
        }

        bucket_end = bucket_start;
    }

    NormalizeSystemTime(bucket_start);
    NormalizeSystemTime(bucket_end);
    return true;
}

bool CHistoryTrafficStore::TryParseBucketTimeRange(const std::wstring& bucket_key, BucketTimeRange& range)
{
    SYSTEMTIME bucket_start{};
    SYSTEMTIME bucket_end{};
    if (!TryParseBucketKey(bucket_key, bucket_start, bucket_end))
    {
        return false;
    }

    const auto start_value = ToFileTimeValue(bucket_start);
    const auto end_value = ToFileTimeValue(bucket_end);
    if (start_value == 0 || end_value == 0)
    {
        return false;
    }

    range.start = start_value;
    range.end = end_value;
    return true;
}

void CHistoryTrafficStore::NormalizeSystemTime(SYSTEMTIME& time)
{
    time.wSecond = 0;
    time.wMilliseconds = 0;
}

bool CHistoryTrafficStore::IsSameRange(const DateTimeRange& left, const DateTimeRange& right)
{
    return ToFileTimeValue(left.start) == ToFileTimeValue(right.start) &&
           ToFileTimeValue(left.end) == ToFileTimeValue(right.end);
}

bool CHistoryTrafficStore::EnsureBucketTimeRange(const std::wstring& bucket_key) const
{
    if (m_bucketTimeRangeByKey.find(bucket_key) != m_bucketTimeRangeByKey.end())
    {
        return true;
    }

    BucketTimeRange range{};
    if (!TryParseBucketTimeRange(bucket_key, range))
    {
        return false;
    }

    m_bucketTimeRangeByKey.emplace(bucket_key, range);
    return true;
}

void CHistoryTrafficStore::InvalidateRangeCache() const
{
    m_rangeCacheValid = false;
    m_cachedRangeApps.clear();
    m_cachedRangeTotal = {};
}

void CHistoryTrafficStore::InvalidateCaches()
{
    InvalidateRangeCache();
    m_allTimeCacheValid = false;
    m_cachedAllTimeTotal = {};
}

ULONGLONG CHistoryTrafficStore::ToFileTimeValue(const SYSTEMTIME& time)
{
    FILETIME file_time{};
    SYSTEMTIME local = time;
    if (SystemTimeToFileTime(&local, &file_time) == FALSE)
    {
        return 0;
    }

    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return value.QuadPart;
}

bool CHistoryTrafficStore::BucketIntersectsRange(const BucketTimeRange& bucket_range, ULONGLONG range_start, ULONGLONG range_end)
{
    return bucket_range.end >= range_start && bucket_range.start <= range_end;
}
