#pragma once

#include <Windows.h>
#include <map>
#include <vector>
#include "TrafficTypes.h"

class CTcpEStatsCollector
{
public:
    struct Cursor
    {
        Traffic::U64 acknowledged{}, received{};
        std::uint32_t sendUnacknowledged{}, sendNext{}, sendMaximum{}, receiveNext{};
    };
    struct Observation
    {
        Traffic::ConnectionKey key;
        Traffic::ProcessIdentity owner;
        Traffic::U64 generation{}, connectionCreated{}, start{}, startLatest{}, endEarliest{}, end{}, rx{}, tx{}, rxDelta{}, txDelta{};
        Traffic::U64 readBeginCounter{}, readEndCounter{};
        Cursor cursor;
        DWORD error{};
        bool baseline{}, closed{}, enabled{}, coarse{};
    };
    struct Diagnostics
    {
        Traffic::U64 polls{}, enableFailures{}, readFailures{}, resets{}, closed{}, pollMicros{}, skipped{}, coarseIntervals{};
        DWORD lastError{}, tableError4{}, tableError6{};
        std::uint32_t connections{}, enabled{}, baselines{};
    };

    const std::vector<Observation>& Poll(bool read_counters = true);
    void Stop();
    const Diagnostics& GetDiagnostics() const { return m_diagnostics; }
    static Traffic::ProcessIdentity QueryOwner(DWORD pid);

private:
    struct State
    {
        Observation value;
        Traffic::U64 seen{}, lastRead{}, lastReadStart{}, retryAfter{};
        bool hasBaseline{}, enabledByUs{};
    };
    void Enumerate(bool ipv6, bool read_counters);
    void Visit(const Traffic::ConnectionKey& key, Traffic::U64 created, bool read_counters);
    static DWORD Read(const Traffic::ConnectionKey& key, Traffic::U64& rx, Traffic::U64& tx, bool& enabled, Cursor& cursor);
    static DWORD Enable(const Traffic::ConnectionKey& key, bool enabled);
    static void ApplyCounters(State& state, Traffic::U64 rx, Traffic::U64 tx,
        Traffic::U64 start, Traffic::U64 end, DWORD error, bool enabled, Diagnostics& diagnostics, const Cursor* cursor = nullptr);

    std::map<Traffic::ConnectionKey, State> m_connections;
    std::map<DWORD, Traffic::ProcessIdentity> m_owners;
    std::vector<unsigned char> m_table4, m_table6;
    std::vector<Observation> m_observations;
    Traffic::U64 m_poll{}, m_generation{};
    bool m_stopping{};
    bool m_includeLoopback{ false };
    Diagnostics m_diagnostics;
};
