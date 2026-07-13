#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
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
        std::wstring exePath;
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
    struct BucketTimeRange
    {
        ULONGLONG start{};
        ULONGLONG end{};
    };

    void EnsureLoaded();
    void Load();
    void ArchiveOldHistoryFiles();
    void LoadHistoryDirectory(const std::wstring& directory, TrafficAmount& loaded_total);
    void AddRangeHistoryFromDirectory(
        const std::wstring& directory,
        const DateTimeRange& range,
        std::unordered_map<std::wstring, TrafficAmount>& totals_by_app) const;
    void AppendHistoryEntries(const std::wstring& bucket_key, const std::vector<std::pair<std::wstring, TrafficAmount>>& entries) const;
    void LoadState();
    void SaveState() const;

    static DateTimeRange GetDefaultRange();
    static SYSTEMTIME GetArchiveCutoff();
    static DateTimeRange NormalizeRange(const DateTimeRange& range);
    static std::wstring GetCurrentMinuteKey();
    static std::wstring FormatMinuteTime(const SYSTEMTIME& time);
    static std::wstring GetDateKeyFromMinuteKey(const std::wstring& bucket_key);
    static std::wstring GetTimeKeyFromMinuteKey(const std::wstring& bucket_key);
    static bool TryParseStoredTime(const std::wstring& text, SYSTEMTIME& time);
    static bool TryParseBucketKey(const std::wstring& bucket_key, SYSTEMTIME& bucket_start, SYSTEMTIME& bucket_end);
    static bool TryParseBucketTimeRange(const std::wstring& bucket_key, BucketTimeRange& range);
    static void NormalizeSystemTime(SYSTEMTIME& time);
    static ULONGLONG ToFileTimeValue(const SYSTEMTIME& time);
    static bool BucketIntersectsRange(const BucketTimeRange& bucket_range, ULONGLONG range_start, ULONGLONG range_end);
    static bool IsSameRange(const DateTimeRange& left, const DateTimeRange& right);
    bool EnsureBucketTimeRange(const std::wstring& bucket_key) const;
    void InvalidateRangeCache() const;
    void InvalidateCaches();

private:
    std::wstring m_baseDir;
    std::wstring m_historyDir;
    std::wstring m_oldHistoryDir;
    std::wstring m_stateFilePath;
    bool m_loaded{ false };
    std::unordered_map<std::wstring, BucketAppMap> m_bucketByApp;
    mutable std::unordered_map<std::wstring, BucketTimeRange> m_bucketTimeRangeByKey;
    std::unordered_map<std::wstring, std::wstring> m_pathByApp;
    DateTimeRange m_preferredRange{};
    DisplayLanguage m_preferredLanguage{ DisplayLanguage::English };
    mutable bool m_rangeCacheValid{ false };
    mutable DateTimeRange m_cachedRange{};
    mutable std::vector<AppTotalEntry> m_cachedRangeApps;
    mutable TrafficAmount m_cachedRangeTotal{};
    mutable bool m_allTimeCacheValid{ false };
    mutable TrafficAmount m_cachedAllTimeTotal{};
    bool m_hasPersistedAllTime{ false };
};
