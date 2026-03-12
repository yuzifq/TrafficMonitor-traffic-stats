#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class CHistoryTrafficStore
{
public:
    enum class PeriodMode
    {
        Day,
        Month,
        Year,
    };

    enum class DisplayLanguage
    {
        English,
        Chinese,
    };

    struct AppTotalEntry
    {
        std::wstring appName;
        std::uint64_t rxTotalBytes{};
        std::uint64_t txTotalBytes{};
    };

    struct TrafficAmount
    {
        std::uint64_t rxBytes{};
        std::uint64_t txBytes{};
    };

    void Initialize(const std::wstring& base_dir);
    void Update(const std::vector<AppTotalEntry>& apps);
    std::vector<AppTotalEntry> GetPeriodAppTotals(PeriodMode period_mode) const;
    TrafficAmount GetPeriodTotal(PeriodMode period_mode) const;
    TrafficAmount GetAllTimeTotal() const;
    PeriodMode GetPreferredPeriodMode() const;
    void SetPreferredPeriodMode(PeriodMode period_mode);
    DisplayLanguage GetPreferredLanguage() const;
    void SetPreferredLanguage(DisplayLanguage language);

private:
    using DailyAppMap = std::unordered_map<std::wstring, TrafficAmount>;

    void EnsureLoaded();
    void Load();
    void Save() const;
    void LoadState();
    void SaveState() const;
    static std::wstring GetTodayKey();
    static std::wstring GetMonthKey();
    static std::wstring GetYearKey();

private:
    std::wstring m_baseDir;
    std::wstring m_filePath;
    std::wstring m_stateFilePath;
    bool m_loaded{ false };
    mutable bool m_dirty{ false };
    std::unordered_map<std::wstring, DailyAppMap> m_dailyByApp;
    std::unordered_map<std::wstring, TrafficAmount> m_lastSeenTotals;
    PeriodMode m_preferredPeriodMode{ PeriodMode::Day };
    DisplayLanguage m_preferredLanguage{ DisplayLanguage::English };
};
