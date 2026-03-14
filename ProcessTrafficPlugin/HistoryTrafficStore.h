#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class CHistoryTrafficStore
{
public:
    enum class DisplayLanguage
    {
        English,
        Chinese,
    };

    struct DateTimeRange
    {
        SYSTEMTIME start{};
        SYSTEMTIME end{};
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
    std::vector<AppTotalEntry> GetRangeAppTotals(const DateTimeRange& range) const;
    TrafficAmount GetRangeTotal(const DateTimeRange& range) const;
    TrafficAmount GetAllTimeTotal() const;
    DateTimeRange GetPreferredRange() const;
    void SetPreferredRange(const DateTimeRange& range);
    DisplayLanguage GetPreferredLanguage() const;
    void SetPreferredLanguage(DisplayLanguage language);

private:
    using BucketAppMap = std::unordered_map<std::wstring, TrafficAmount>;

    void EnsureLoaded();
    void Load();
    void Save() const;
    void LoadState();
    void SaveState() const;

    static DateTimeRange GetDefaultRange();
    static DateTimeRange NormalizeRange(const DateTimeRange& range);
    static std::wstring GetCurrentMinuteKey();
    static std::wstring FormatMinuteTime(const SYSTEMTIME& time);
    static bool TryParseStoredTime(const std::wstring& text, SYSTEMTIME& time);
    static bool TryParseBucketKey(const std::wstring& bucket_key, SYSTEMTIME& bucket_start, SYSTEMTIME& bucket_end);
    static void NormalizeSystemTime(SYSTEMTIME& time);
    static ULONGLONG ToFileTimeValue(const SYSTEMTIME& time);
    static bool BucketIntersectsRange(const std::wstring& bucket_key, const DateTimeRange& range);

private:
    std::wstring m_baseDir;
    std::wstring m_filePath;
    std::wstring m_stateFilePath;
    bool m_loaded{ false };
    mutable bool m_dirty{ false };
    std::unordered_map<std::wstring, BucketAppMap> m_bucketByApp;
    std::unordered_map<std::wstring, TrafficAmount> m_lastSeenTotals;
    DateTimeRange m_preferredRange{};
    DisplayLanguage m_preferredLanguage{ DisplayLanguage::English };
};
