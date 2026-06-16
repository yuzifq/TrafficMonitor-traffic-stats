#include "HistoryTrafficStore.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

namespace
{
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

bool IsSameAmount(const CHistoryTrafficStore::TrafficAmount& left, const CHistoryTrafficStore::TrafficAmount& right)
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

bool TryReadDailyHistoryLine(
    const std::wstring& date_key,
    const std::wstring& line,
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
    if (!TryParseUInt64(rx_text, rx) || !TryParseUInt64(tx_text, tx))
    {
        return false;
    }

    const std::wstring_view time_key(line.data(), first_tab);
    if (time_key.size() != 5)
    {
        return false;
    }

    bucket_key = date_key;
    bucket_key += L" ";
    bucket_key.append(time_key);
    app_name.assign(line.data() + first_tab + 1, second_tab - first_tab - 1);
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

    amount.rxBytes = _wcstoui64(rx_text.c_str(), nullptr, 10);
    amount.txBytes = _wcstoui64(tx_text.c_str(), nullptr, 10);
    return true;
}

void WriteStateLine(std::wofstream& output, const wchar_t* key, const std::wstring& value)
{
    output << key << L'\t' << value << L'\n';
}

void WriteStateLine(std::wofstream& output, const wchar_t* key, int value)
{
    output << key << L'\t' << value << L'\n';
}

void WriteLastSeenEntry(std::wofstream& output, const std::wstring& app_name, const CHistoryTrafficStore::TrafficAmount& amount)
{
    output << L"last\t"
           << app_name << L'\t'
           << amount.rxBytes << L'\t'
           << amount.txBytes << L'\n';
}

void WritePathEntry(std::wofstream& output, const std::wstring& app_name, const std::wstring& exe_path)
{
    output << L"path\t" << app_name << L'\t' << exe_path << L'\n';
}

bool WriteDailyHistoryEntry(
    std::wofstream& output,
    const std::wstring& time_key,
    const std::wstring& app_name,
    const CHistoryTrafficStore::TrafficAmount& amount)
{
    output << time_key << L'\t'
           << app_name << L'\t'
           << amount.rxBytes << L'\t'
           << amount.txBytes << L'\n';
    return static_cast<bool>(output);
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
    m_stateFilePath = (std::filesystem::path(m_baseDir) / L"app_traffic_state.tsv").wstring();
    m_loaded = false;
    m_pathByApp.clear();
    m_lastSeenTotals.clear();
    InvalidateCaches();
}

void CHistoryTrafficStore::Update(const std::vector<AppTotalEntry>& apps)
{
    EnsureLoaded();
    const auto bucket_key = GetCurrentMinuteKey();
    std::vector<std::pair<std::wstring, TrafficAmount>> delta_entries;
    TrafficAmount total_delta{};
    bool history_dirty = false;
    bool state_dirty = false;
    bool path_dirty = false;

    for (const auto& app : apps)
    {
        const auto current = MakeAmount(app.rxTotalBytes, app.txTotalBytes);
        auto previous_it = m_lastSeenTotals.find(app.appName);
        auto delta = current;
        if (previous_it != m_lastSeenTotals.end())
        {
            delta.rxBytes = current.rxBytes >= previous_it->second.rxBytes ? current.rxBytes - previous_it->second.rxBytes : 0;
            delta.txBytes = current.txBytes >= previous_it->second.txBytes ? current.txBytes - previous_it->second.txBytes : 0;
        }

        if (delta.rxBytes != 0 || delta.txBytes != 0)
        {
            delta_entries.emplace_back(app.appName, delta);
            AddAmount(total_delta, delta);
            history_dirty = true;
        }

        if (!app.exePath.empty())
        {
            auto path_it = m_pathByApp.find(app.appName);
            if (path_it == m_pathByApp.end())
            {
                m_pathByApp.emplace(app.appName, app.exePath);
                state_dirty = true;
                path_dirty = true;
            }
            else if (path_it->second != app.exePath)
            {
                path_it->second = app.exePath;
                state_dirty = true;
                path_dirty = true;
            }
        }

        if (previous_it == m_lastSeenTotals.end())
        {
            m_lastSeenTotals.emplace(app.appName, current);
            state_dirty = true;
        }
        else if (!IsSameAmount(previous_it->second, current))
        {
            previous_it->second = current;
            state_dirty = true;
        }
    }

    if (history_dirty)
    {
        AppendHistoryEntries(bucket_key, delta_entries);
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

    if (state_dirty)
    {
        SaveState();
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
    TrafficAmount total{};
    LoadRangeDailyHistoryFiles(normalized, totals_by_app, total);

    std::vector<AppTotalEntry> result;
    result.reserve(totals_by_app.size());
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
    LoadDailyHistoryFiles(total);
    m_cachedAllTimeTotal = total;
    m_allTimeCacheValid = true;
    return total;
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
        m_stateFilePath = (std::filesystem::path(m_baseDir) / L"app_traffic_state.tsv").wstring();
    }

    Load();
    LoadState();
    m_loaded = true;
}

void CHistoryTrafficStore::Load()
{
    m_pathByApp.clear();
    InvalidateCaches();
}

void CHistoryTrafficStore::LoadDailyHistoryFiles(TrafficAmount& loaded_total) const
{
    if (m_historyDir.empty() || !std::filesystem::exists(m_historyDir))
    {
        return;
    }

    for (const auto& file_entry : std::filesystem::directory_iterator(std::filesystem::path(m_historyDir)))
    {
        if (!file_entry.is_regular_file())
        {
            continue;
        }

        const auto path = file_entry.path();
        if (path.extension() != L".tsv")
        {
            continue;
        }

        const auto date_key = path.stem().wstring();
        if (date_key.empty())
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
        while (std::getline(input, line))
        {
            std::wstring bucket_key;
            std::wstring app_name;
            TrafficAmount amount{};
            BucketTimeRange bucket_range{};
            if (!TryReadDailyHistoryLine(date_key, line, bucket_key, app_name, amount) ||
                !TryParseBucketTimeRange(bucket_key, bucket_range))
            {
                continue;
            }

            AddAmount(loaded_total, amount);
        }
    }
}

void CHistoryTrafficStore::LoadRangeDailyHistoryFiles(
    const DateTimeRange& range,
    BucketAppMap& totals_by_app,
    TrafficAmount& total) const
{
    if (m_historyDir.empty() || !std::filesystem::exists(m_historyDir))
    {
        return;
    }

    const auto range_start = ToFileTimeValue(range.start);
    const auto range_end = ToFileTimeValue(range.end);
    for (const auto& file_entry : std::filesystem::directory_iterator(std::filesystem::path(m_historyDir)))
    {
        if (!file_entry.is_regular_file())
        {
            continue;
        }

        const auto path = file_entry.path();
        if (path.extension() != L".tsv")
        {
            continue;
        }

        const auto date_key = path.stem().wstring();
        SYSTEMTIME day_start{};
        SYSTEMTIME day_end{};
        if (!TryParseBucketKey(date_key, day_start, day_end))
        {
            continue;
        }

        const BucketTimeRange day_range{ ToFileTimeValue(day_start), ToFileTimeValue(day_end) };
        if (!BucketIntersectsRange(day_range, range_start, range_end))
        {
            continue;
        }

        std::wifstream input{ path };
        std::wstring line;
        while (std::getline(input, line))
        {
            std::wstring bucket_key;
            std::wstring app_name;
            TrafficAmount amount{};
            BucketTimeRange bucket_range{};
            if (!TryReadDailyHistoryLine(date_key, line, bucket_key, app_name, amount) ||
                !TryParseBucketTimeRange(bucket_key, bucket_range) ||
                !BucketIntersectsRange(bucket_range, range_start, range_end))
            {
                continue;
            }

            AddAmount(totals_by_app[app_name], amount);
            AddAmount(total, amount);
        }
    }
}

void CHistoryTrafficStore::AppendHistoryEntries(
    const std::wstring& bucket_key,
    const std::vector<std::pair<std::wstring, TrafficAmount>>& entries) const
{
    if (m_historyDir.empty() || entries.empty())
    {
        return;
    }

    const auto date_key = GetDateKeyFromMinuteKey(bucket_key);
    const auto time_key = GetTimeKeyFromMinuteKey(bucket_key);
    if (date_key.empty() || time_key.empty())
    {
        return;
    }

    std::filesystem::create_directories(std::filesystem::path(m_historyDir));
    const auto file_path = std::filesystem::path(m_historyDir) / (date_key + L".tsv");
    std::wofstream output{ file_path, std::ios::app };
    if (!output.is_open())
    {
        return;
    }

    for (const auto& entry : entries)
    {
        if (!WriteDailyHistoryEntry(output, time_key, entry.first, entry.second))
        {
            break;
        }
    }
}

void CHistoryTrafficStore::LoadState()
{
    m_lastSeenTotals.clear();
    m_pathByApp.clear();
    InvalidateRangeCache();
    m_preferredRange = GetDefaultRange();
    m_preferredLanguage = DisplayLanguage::English;

    if (m_stateFilePath.empty() || !std::filesystem::exists(m_stateFilePath))
    {
        return;
    }

    std::wifstream input{ std::filesystem::path(m_stateFilePath) };
    std::wstring line;
    while (std::getline(input, line))
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

        if (type != L"last")
        {
            continue;
        }

        std::wstring app_name;
        if (!ReadTabField(stream, app_name))
        {
            continue;
        }

        auto& entry = m_lastSeenTotals[app_name];
        if (!ReadStoredAmount(stream, entry))
        {
            m_lastSeenTotals.erase(app_name);
        }
    }

    m_preferredRange = NormalizeRange(m_preferredRange);
}

void CHistoryTrafficStore::SaveState() const
{
    if (m_stateFilePath.empty())
    {
        return;
    }

    const auto normalized = NormalizeRange(m_preferredRange);
    std::wofstream output{ std::filesystem::path(m_stateFilePath), std::ios::trunc };
    WriteStateLine(output, L"language", static_cast<int>(m_preferredLanguage));
    WriteStateLine(output, L"range_start", FormatMinuteTime(normalized.start));
    WriteStateLine(output, L"range_end", FormatMinuteTime(normalized.end));
    for (const auto& entry : m_pathByApp)
    {
        if (!entry.first.empty() && !entry.second.empty())
        {
            WritePathEntry(output, entry.first, entry.second);
        }
    }
    for (const auto& entry : m_lastSeenTotals)
    {
        WriteLastSeenEntry(output, entry.first, entry.second);
    }
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
