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
    const auto start_key = ToMinuteKey(normalized.start);
    const auto end_key = ToMinuteKey(normalized.end);

    std::unordered_map<std::wstring, TrafficAmount> totals_by_app;
    for (const auto& bucket_entry : m_bucketByApp)
    {
        std::wstring bucket_key = bucket_entry.first;
        if (bucket_key.size() == 10)
        {
            bucket_key += L" 00:00";
        }

        if (bucket_key < start_key || bucket_key > end_key)
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
            if (std::getline(stream, value_text, L'\t'))
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
            if (std::getline(stream, value_text, L'\t'))
            {
                TryParseStoredTime(value_text, m_preferredRange.start);
            }
            continue;
        }

        if (type == L"range_end")
        {
            std::wstring value_text;
            if (std::getline(stream, value_text, L'\t'))
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
        std::wstring rx_text;
        std::wstring tx_text;
        if (!std::getline(stream, app_name, L'\t') ||
            !std::getline(stream, rx_text, L'\t') ||
            !std::getline(stream, tx_text, L'\t'))
        {
            continue;
        }

        auto& entry = m_lastSeenTotals[app_name];
        entry.rxBytes = _wcstoui64(rx_text.c_str(), nullptr, 10);
        entry.txBytes = _wcstoui64(tx_text.c_str(), nullptr, 10);
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
    output << L"language\t" << static_cast<int>(m_preferredLanguage) << L'\n';
    output << L"range_start\t" << ToStateText(normalized.start) << L'\n';
    output << L"range_end\t" << ToStateText(normalized.end) << L'\n';
    for (const auto& entry : m_lastSeenTotals)
    {
        output << L"last\t"
               << entry.first << L'\t'
               << entry.second.rxBytes << L'\t'
               << entry.second.txBytes << L'\n';
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
    normalized.start.wSecond = 0;
    normalized.start.wMilliseconds = 0;
    normalized.end.wSecond = 0;
    normalized.end.wMilliseconds = 0;

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
    return ToMinuteKey(local_time);
}

std::wstring CHistoryTrafficStore::ToMinuteKey(const SYSTEMTIME& time)
{
    wchar_t buffer[20]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute);
    return buffer;
}

std::wstring CHistoryTrafficStore::ToStateText(const SYSTEMTIME& time)
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

    parsed.wSecond = 0;
    parsed.wMilliseconds = 0;
    time = parsed;
    return true;
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
