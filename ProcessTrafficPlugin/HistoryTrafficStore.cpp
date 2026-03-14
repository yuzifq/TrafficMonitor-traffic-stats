#include "HistoryTrafficStore.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

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
}

void CHistoryTrafficStore::Initialize(const std::wstring& base_dir)
{
    if (base_dir.empty())
    {
        return;
    }

    m_baseDir = base_dir;
    std::filesystem::create_directories(std::filesystem::path(m_baseDir));
    m_filePath = (std::filesystem::path(m_baseDir) / L"app_traffic_history.tsv").wstring();
    m_stateFilePath = (std::filesystem::path(m_baseDir) / L"app_traffic_state.tsv").wstring();
}

void CHistoryTrafficStore::Update(const std::vector<AppTotalEntry>& apps)
{
    EnsureLoaded();
    const auto bucket_key = GetCurrentMinuteKey();
    auto& bucket = m_bucketByApp[bucket_key];

    for (const auto& app : apps)
    {
        const auto current = MakeAmount(app.rxTotalBytes, app.txTotalBytes);
        const auto previous_it = m_lastSeenTotals.find(app.appName);
        auto delta = current;
        if (previous_it != m_lastSeenTotals.end())
        {
            delta.rxBytes = current.rxBytes >= previous_it->second.rxBytes ? current.rxBytes - previous_it->second.rxBytes : 0;
            delta.txBytes = current.txBytes >= previous_it->second.txBytes ? current.txBytes - previous_it->second.txBytes : 0;
        }

        if (delta.rxBytes != 0 || delta.txBytes != 0)
        {
            auto& stored = bucket[app.appName];
            stored.rxBytes += delta.rxBytes;
            stored.txBytes += delta.txBytes;
            m_dirty = true;
        }

        m_lastSeenTotals[app.appName] = current;
    }

    if (m_dirty)
    {
        Save();
        m_dirty = false;
    }
    SaveState();
}

std::vector<CHistoryTrafficStore::AppTotalEntry> CHistoryTrafficStore::GetRangeAppTotals(const DateTimeRange& range) const
{
    const_cast<CHistoryTrafficStore*>(this)->EnsureLoaded();
    const auto normalized = NormalizeRange(range);

    std::unordered_map<std::wstring, TrafficAmount> totals_by_app;
    for (const auto& bucket_entry : m_bucketByApp)
    {
        if (!BucketIntersectsRange(bucket_entry.first, normalized))
        {
            continue;
        }

        for (const auto& app_entry : bucket_entry.second)
        {
            AddAmount(totals_by_app[app_entry.first], app_entry.second);
        }
    }

    std::vector<AppTotalEntry> result;
    result.reserve(totals_by_app.size());
    for (const auto& entry : totals_by_app)
    {
        AppTotalEntry item{};
        item.appName = entry.first;
        item.rxTotalBytes = entry.second.rxBytes;
        item.txTotalBytes = entry.second.txBytes;
        result.push_back(item);
    }
    return result;
}

CHistoryTrafficStore::TrafficAmount CHistoryTrafficStore::GetRangeTotal(const DateTimeRange& range) const
{
    TrafficAmount total{};
    const auto apps = GetRangeAppTotals(range);
    for (const auto& app : apps)
    {
        total.rxBytes += app.rxTotalBytes;
        total.txBytes += app.txTotalBytes;
    }
    return total;
}

CHistoryTrafficStore::TrafficAmount CHistoryTrafficStore::GetAllTimeTotal() const
{
    const_cast<CHistoryTrafficStore*>(this)->EnsureLoaded();

    TrafficAmount total{};
    for (const auto& bucket_entry : m_bucketByApp)
    {
        for (const auto& app_entry : bucket_entry.second)
        {
            AddAmount(total, app_entry.second);
        }
    }
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
    m_preferredRange = NormalizeRange(range);
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
        m_filePath = (std::filesystem::path(m_baseDir) / L"app_traffic_history.tsv").wstring();
        m_stateFilePath = (std::filesystem::path(m_baseDir) / L"app_traffic_state.tsv").wstring();
    }

    Load();
    LoadState();
    m_loaded = true;
}

void CHistoryTrafficStore::Load()
{
    m_bucketByApp.clear();
    if (m_filePath.empty() || !std::filesystem::exists(m_filePath))
    {
        return;
    }

    std::wifstream input{ std::filesystem::path(m_filePath) };
    std::wstring line;
    while (std::getline(input, line))
    {
        std::wistringstream stream(line);
        std::wstring bucket_key;
        std::wstring app_name;
        std::wstring rx_text;
        std::wstring tx_text;
        if (!std::getline(stream, bucket_key, L'\t') ||
            !std::getline(stream, app_name, L'\t') ||
            !std::getline(stream, rx_text, L'\t') ||
            !std::getline(stream, tx_text, L'\t'))
        {
            continue;
        }

        auto& entry = m_bucketByApp[bucket_key][app_name];
        entry.rxBytes = _wcstoui64(rx_text.c_str(), nullptr, 10);
        entry.txBytes = _wcstoui64(tx_text.c_str(), nullptr, 10);
    }
}

void CHistoryTrafficStore::Save() const
{
    if (m_filePath.empty())
    {
        return;
    }

    std::wofstream output{ std::filesystem::path(m_filePath), std::ios::trunc };
    for (const auto& bucket_entry : m_bucketByApp)
    {
        for (const auto& app_entry : bucket_entry.second)
        {
            output << bucket_entry.first << L'\t'
                   << app_entry.first << L'\t'
                   << app_entry.second.rxBytes << L'\t'
                   << app_entry.second.txBytes << L'\n';
        }
    }
}

void CHistoryTrafficStore::LoadState()
{
    m_lastSeenTotals.clear();
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

void CHistoryTrafficStore::NormalizeSystemTime(SYSTEMTIME& time)
{
    time.wSecond = 0;
    time.wMilliseconds = 0;
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

bool CHistoryTrafficStore::BucketIntersectsRange(const std::wstring& bucket_key, const DateTimeRange& range)
{
    SYSTEMTIME bucket_start{};
    SYSTEMTIME bucket_end{};
    if (!TryParseBucketKey(bucket_key, bucket_start, bucket_end))
    {
        return false;
    }

    const auto range_start = ToFileTimeValue(range.start);
    const auto range_end = ToFileTimeValue(range.end);
    const auto bucket_start_value = ToFileTimeValue(bucket_start);
    const auto bucket_end_value = ToFileTimeValue(bucket_end);

    return bucket_end_value >= range_start && bucket_start_value <= range_end;
}
