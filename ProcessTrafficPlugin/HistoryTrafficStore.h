#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "TrafficTypes.h"

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
        std::wstring identity;
        std::uint64_t sampleStart{}, sampleEnd{};
        Traffic::Source source{ Traffic::Source::Etw };
        Traffic::Confidence confidence{ Traffic::Confidence::Normal };
    };

    struct TrafficAmount
    {
        std::uint64_t rxBytes{};
        std::uint64_t txBytes{};
    };
    struct EngineMetadata
    {
        std::uint64_t cutover{}, firstEvent{}, lastEvent{}, minutes{};
        unsigned sources{}, confidence{};
        bool corrupt{}, legacy{};
    };

    void Initialize(const std::wstring& base_dir);
    std::wstring GetStorageDirectory() const;
    bool Update(const std::vector<AppTotalEntry>& apps);
    bool Checkpoint(bool force = false);
    bool IsWriteHealthy() const { return m_writeHealthy; }
    bool IsReady() const { return m_loaded; }
    bool IsRangeHealthy(const DateTimeRange& range) const { GetRangeTotal(range); return m_loaded && m_rangeHealthy; }
    bool IsAllTimeHealthy() const { return m_loaded && m_historyReadHealthy; }
    EngineMetadata GetEngineMetadata(const DateTimeRange& range) const;
    std::vector<AppTotalEntry> GetRangeAppTotals(const DateTimeRange& range) const;
    TrafficAmount GetRangeTotal(const DateTimeRange& range) const;
    TrafficAmount GetAllTimeTotal() const;
    bool ExportRange(const DateTimeRange& range, const std::wstring& output_path, std::wstring& error_message) const;
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
    bool RequiresAppDictionary() const;
    bool EnsureCompletedSummaries();
    void Load();
    TrafficAmount CalculateStoredHistoryTotal() const;
    bool LoadAppDictionary();
    bool SaveAppDictionary() const;
    std::uint64_t EnsureAppId(const std::wstring& app_name);
    void LoadHistoryFile(const std::wstring& path, TrafficAmount& loaded_total);
    bool AddMinuteFileTotals(
        const std::wstring& path,
        const DateTimeRange& range,
        std::unordered_map<std::wstring, TrafficAmount>& totals_by_app) const;
    bool AppendHistoryEntries(const std::wstring& bucket_key, const std::vector<std::pair<std::wstring, TrafficAmount>>& entries);
    void LoadCheckpoint();
    bool SaveCheckpoint();
    bool EnsureCommitId();
    bool FlushCurrentMinute();
    bool AppendEngineMetadata();
    bool RecoverAppend();
    void ClearCheckpoint() const;
    void LoadState();
    bool SaveState() const;

    static DateTimeRange GetDefaultRange();
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
    std::wstring m_appDictionaryPath;
    std::wstring m_stateFilePath;
    std::wstring m_checkpointFilePath;
    std::wstring m_checkpointCommitPath;
    bool m_loaded{ false };
    std::unordered_map<std::wstring, BucketAppMap> m_bucketByApp;
    std::wstring m_currentMinuteKey;
    BucketAppMap m_currentMinuteTotals;
    std::uint64_t m_currentCommitId{};
    bool m_legacyCheckpoint{}, m_checkpointBlocked{};
    ULONGLONG m_lastCheckpointTick{};
    mutable std::unordered_map<std::wstring, BucketTimeRange> m_bucketTimeRangeByKey;
    std::unordered_map<std::wstring, std::uint64_t> m_appIdByName;
    std::unordered_map<std::uint64_t, std::wstring> m_appNameById;
    std::uint64_t m_nextAppId{ 1 };
    std::unordered_map<std::wstring, std::wstring> m_pathByApp;
    DateTimeRange m_preferredRange{};
    DisplayLanguage m_preferredLanguage{ DisplayLanguage::English };
    int m_historyMigrationFormat{};
    int m_allTimeRecoveryFormat{};
    mutable bool m_rangeCacheValid{ false };
    mutable bool m_rangeHealthy{ true }, m_historyReadHealthy{ true };
    bool m_currentDayHealthy{ true };
    mutable DateTimeRange m_cachedRange{};
    mutable std::vector<AppTotalEntry> m_cachedRangeApps;
    mutable TrafficAmount m_cachedRangeTotal{};
    mutable bool m_allTimeCacheValid{ false };
    mutable TrafficAmount m_cachedAllTimeTotal{};
    bool m_hasPersistedAllTime{ false };
    bool m_checkpointDirty{}, m_stateDirty{}, m_writeHealthy{ true };
    EngineMetadata m_engine, m_currentEngine;
};
