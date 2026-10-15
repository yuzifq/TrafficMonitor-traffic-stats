#pragma once

#include "HistoryTrafficStore.h"
#include "NetworkTrafficCollector.h"

#include <deque>
#include <filesystem>
#include <map>
#include <optional>

class CNetworkTrafficStore
{
public:
    using Amount = CHistoryTrafficStore::TrafficAmount;
    using Range = CHistoryTrafficStore::DateTimeRange;
    struct Query
    {
        Amount amount{};
        Amount boundary{};
        std::uint64_t coveredMs{};
        std::uint64_t requestedMs{};
        bool available{};
        bool coarse{};
        bool healthy{ true };
    };

    void Initialize(const std::wstring& base_dir);
    bool Add(const CNetworkTrafficCollector::Interval& interval);
    bool Flush(bool finish_current = false);
    Query GetRange(const Range& range) const;
    Query GetRangeUtc(std::uint64_t start_ms, std::uint64_t end_ms) const;
    Amount GetAllTimeTotal(bool* healthy = nullptr) const;
    bool ExportRange(const Range& range, const std::wstring& path, std::wstring& error) const;
    const std::wstring& GetPreferredAdapter() const { return m_preferredAdapter; }
    bool SetPreferredAdapter(const std::wstring& id);
    bool Healthy() const { return m_writeHealthy; }
    bool IsManagedPath(const std::wstring& path) const;
    static bool RangeBounds(const Range& range, std::uint64_t& start_ms, std::uint64_t& end_ms);

private:
    struct Record : CNetworkTrafficCollector::Interval { std::uint64_t id{}; };
    struct Day
    {
        std::uint64_t first{}, last{}, rx{}, tx{}, covered{}, lastId{}, count{};
        std::uintmax_t size{};
        std::filesystem::file_time_type writeTime{};
        bool coarse{}, healthy{ true };
    };
    static std::wstring DayKey(std::uint64_t utc_ms);
    std::filesystem::path DayPath(const std::wstring& key) const;
    static std::string Encode(const Record& record);
    static bool Decode(const std::string& line, Record& record);
    static Day Summarize(const std::vector<Record>& records);
    bool ReadDay(const std::wstring& key, std::vector<Record>& records, std::uint64_t* valid_bytes = nullptr,
        bool* repairable = nullptr) const;
    bool RefreshDay(const std::wstring& key) const;
    bool ReadSummary(const std::wstring& key, Day& day) const;
    void SaveSummary(const std::wstring& key, const Day& day) const;
    bool SaveCheckpoint();
    bool Append(const Record& record);
    void RecoverCheckpoint();

    std::filesystem::path m_root;
    mutable std::map<std::wstring, Day> m_days;
    mutable std::wstring m_cachedDay;
    mutable std::vector<Record> m_cachedRecords;
    mutable std::uintmax_t m_cachedSize{};
    mutable std::filesystem::file_time_type m_cachedWrite{};
    std::deque<Record> m_pending;
    std::optional<Record> m_current;
    std::wstring m_preferredAdapter;
    std::uint64_t m_nextId{ 1 };
    ULONGLONG m_lastCheckpoint{};
    bool m_dirty{};
    bool m_writeHealthy{ true };
    bool m_checkpointCorrupt{};
};
