#pragma once

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class CNetworkTrafficCollector
{
public:
    struct Adapter
    {
        std::wstring id;
        std::wstring name;
        std::uint64_t instance{};
        std::uint64_t rxBytes{};
        std::uint64_t txBytes{};
        bool connected{};
        bool hardware{};
    };
    struct Interval
    {
        std::uint64_t startMs{};
        std::uint64_t endMs{};
        std::uint64_t rxBytes{};
        std::uint64_t txBytes{};
        std::wstring adapterId;
        bool coarse{};
    };
    enum class Status { Starting, Running, Disconnected, Error };
    struct Snapshot
    {
        std::vector<Adapter> adapters;
        std::wstring preferredId;
        std::wstring activeName;
        std::uint64_t rxPerSec{};
        std::uint64_t txPerSec{};
        Status status{ Status::Starting };
    };

    void SelectAdapter(const std::wstring& id);
    std::optional<Interval> Sample();
    std::optional<Interval> Observe(std::vector<Adapter> adapters, std::uint64_t tick, std::uint64_t utc_ms);
    const Snapshot& GetSnapshot() const { return m_snapshot; }
    static std::uint64_t UtcNowMs();

private:
    Snapshot m_snapshot;
    Adapter m_previous;
    std::uint64_t m_previousTick{};
    std::uint64_t m_previousUtc{};
    std::uint64_t m_idleSince{};
    bool m_hasBaseline{};
};
