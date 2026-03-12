#include "HistoryTrafficStore.h"

#include <Windows.h>

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

std::wstring GetCurrentDateKey(const wchar_t* format)
{
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    wchar_t buffer[16]{};
    swprintf_s(buffer, format, local_time.wYear, local_time.wMonth, local_time.wDay);
    return buffer;
}

bool MatchesPeriod(const std::wstring& date_key, CHistoryTrafficStore::PeriodMode period_mode)
{
    const std::wstring today_key = GetCurrentDateKey(L"%04u-%02u-%02u");
    const std::wstring month_key = today_key.substr(0, 7);
    const std::wstring year_key = today_key.substr(0, 4);

    switch (period_mode)
    {
    case CHistoryTrafficStore::PeriodMode::Day:
        return date_key == today_key;
    case CHistoryTrafficStore::PeriodMode::Month:
        return date_key.rfind(month_key, 0) == 0;
    case CHistoryTrafficStore::PeriodMode::Year:
        return date_key.rfind(year_key, 0) == 0;
    default:
        return false;
    }
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
    const auto today = GetTodayKey();
    auto& daily = m_dailyByApp[today];

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
            auto& stored = daily[app.appName];
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

std::vector<CHistoryTrafficStore::AppTotalEntry> CHistoryTrafficStore::GetPeriodAppTotals(PeriodMode period_mode) const
{
    const_cast<CHistoryTrafficStore*>(this)->EnsureLoaded();

    std::unordered_map<std::wstring, TrafficAmount> totals_by_app;
    for (const auto& day_entry : m_dailyByApp)
    {
        if (!MatchesPeriod(day_entry.first, period_mode))
        {
            continue;
        }
        for (const auto& app_entry : day_entry.second)
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

CHistoryTrafficStore::TrafficAmount CHistoryTrafficStore::GetPeriodTotal(PeriodMode period_mode) const
{
    TrafficAmount total{};
    const auto apps = GetPeriodAppTotals(period_mode);
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
    for (const auto& day_entry : m_dailyByApp)
    {
        for (const auto& app_entry : day_entry.second)
        {
            AddAmount(total, app_entry.second);
        }
    }
    return total;
}

CHistoryTrafficStore::PeriodMode CHistoryTrafficStore::GetPreferredPeriodMode() const
{
    const_cast<CHistoryTrafficStore*>(this)->EnsureLoaded();
    return m_preferredPeriodMode;
}

void CHistoryTrafficStore::SetPreferredPeriodMode(PeriodMode period_mode)
{
    EnsureLoaded();
    if (m_preferredPeriodMode == period_mode)
    {
        return;
    }

    m_preferredPeriodMode = period_mode;
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
    m_dailyByApp.clear();
    if (m_filePath.empty() || !std::filesystem::exists(m_filePath))
    {
        return;
    }

    std::wifstream input{ std::filesystem::path(m_filePath) };
    std::wstring line;
    while (std::getline(input, line))
    {
        std::wistringstream stream(line);
        std::wstring date_key;
        std::wstring app_name;
        std::wstring rx_text;
        std::wstring tx_text;
        if (!std::getline(stream, date_key, L'\t') ||
            !std::getline(stream, app_name, L'\t') ||
            !std::getline(stream, rx_text, L'\t') ||
            !std::getline(stream, tx_text, L'\t'))
        {
            continue;
        }

        auto& entry = m_dailyByApp[date_key][app_name];
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
    for (const auto& day_entry : m_dailyByApp)
    {
        for (const auto& app_entry : day_entry.second)
        {
            output << day_entry.first << L'\t'
                   << app_entry.first << L'\t'
                   << app_entry.second.rxBytes << L'\t'
                   << app_entry.second.txBytes << L'\n';
        }
    }
}

void CHistoryTrafficStore::LoadState()
{
    m_lastSeenTotals.clear();
    m_preferredPeriodMode = PeriodMode::Day;
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

        if (type == L"period")
        {
            std::wstring value_text;
            if (!std::getline(stream, value_text, L'\t'))
            {
                continue;
            }

            const int value = _wtoi(value_text.c_str());
            if (value >= static_cast<int>(PeriodMode::Day) && value <= static_cast<int>(PeriodMode::Year))
            {
                m_preferredPeriodMode = static_cast<PeriodMode>(value);
            }
            continue;
        }

        if (type == L"language")
        {
            std::wstring value_text;
            if (!std::getline(stream, value_text, L'\t'))
            {
                continue;
            }

            const int value = _wtoi(value_text.c_str());
            if (value >= static_cast<int>(DisplayLanguage::English) && value <= static_cast<int>(DisplayLanguage::Chinese))
            {
                m_preferredLanguage = static_cast<DisplayLanguage>(value);
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
}

void CHistoryTrafficStore::SaveState() const
{
    if (m_stateFilePath.empty())
    {
        return;
    }

    std::wofstream output{ std::filesystem::path(m_stateFilePath), std::ios::trunc };
    output << L"period\t" << static_cast<int>(m_preferredPeriodMode) << L'\n';
    output << L"language\t" << static_cast<int>(m_preferredLanguage) << L'\n';
    for (const auto& entry : m_lastSeenTotals)
    {
        output << L"last\t"
               << entry.first << L'\t'
               << entry.second.rxBytes << L'\t'
               << entry.second.txBytes << L'\n';
    }
}

std::wstring CHistoryTrafficStore::GetTodayKey()
{
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%04u-%02u-%02u", local_time.wYear, local_time.wMonth, local_time.wDay);
    return buffer;
}

std::wstring CHistoryTrafficStore::GetMonthKey()
{
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%04u-%02u", local_time.wYear, local_time.wMonth);
    return buffer;
}

std::wstring CHistoryTrafficStore::GetYearKey()
{
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%04u", local_time.wYear);
    return buffer;
}



