#pragma once

#include <map>
#include <vector>
#include "TcpProofCollector.h"
#include "TcpEStatsCollector.h"

// Serialized by the hybrid collector's mutex. Each switched connection owns
// one prefix ledger; late legacy callbacks are suppressed by lifetime windows.
class CTcpSourceTracker
{
public:
    struct Slice
    {
        Traffic::ConnectionKey key;
        Traffic::ProcessIdentity owner;
        Traffic::U64 generation{}, rx{}, tx{}, start{}, end{};
        Traffic::Source source{ Traffic::Source::Etw };
        Traffic::Confidence confidence{ Traffic::Confidence::Normal };
    };
    struct Diagnostics
    {
        Traffic::U64 switches{}, fallbacks{}, rejected{}, overflows{}, gaps{}, estatsRx{}, estatsTx{},
            suppressedRx{}, suppressedTx{};
        unsigned active{}, probing{}, uncertain{}, windows{};
    };
    void Reset();
    void SetHealth(bool proofHealthy, ULONG legacyClock, bool legacyHealthy = true);
    void OnProof(const CTcpProofCollector::Event& event);
    bool KeepLegacy(const Traffic::NetworkEvent& event);
    void Observe(const CTcpEStatsCollector::Observation& sample);
    std::vector<Slice> TakeSlices();
    void Expire();
    void Finish();
    Diagnostics GetDiagnostics() const;

private:
    using U64 = Traffic::U64;
    using Span = std::pair<U64, U64>;
    struct Direction
    {
        U64 prefix{}, highest{}, confirmed{}, seenPrefix{};
        std::vector<Span> pending, credited, seen;
    };
    struct Connection
    {
        Traffic::ConnectionKey key;
        Traffic::ProcessIdentity owner;
        U64 tcb{}, generation{}, begin{}, bornUtc{}, utc{}, established{}, closed{}, tick{}, estatsGeneration{}, contextCreated{},
            probeEnd{}, probeRx{}, probeTx{}, probeUniqueRx{}, probeUniqueTx{}, readEnd{}, lastUtc{}, rxFin{},
            rxThroughOffset{}, txThroughOffset{}, lastDataRx{}, lastDataTx{}, readTick{};
        std::uint32_t sendOrigin{}, receiveOrigin{};
        Traffic::ConnectionState state{ Traffic::ConnectionState::EtwOnly };
        Direction rx, tx;
        bool connected{}, haveSend{}, haveReceive{}, eligible{ true }, switched{}, haveFin{}, frozen{};
    };
    struct Window { U64 begin{}, end{}; };
    struct Legacy { U64 begin{}, counter{}, tick{}, rx{}, tx{}; bool unmapped{}; };
    static bool Add(std::vector<Span>& spans, U64 left, U64 right, U64& added);
    static bool Position(std::uint32_t sequence, std::uint32_t origin, U64 prefix, U64& position);
    static bool Credit(Direction& direction, U64 left, U64 right, U64& bytes);
    void Emit(Connection& connection, U64 rx, U64 tx, U64 start, U64 end,
        Traffic::Source source, Traffic::Confidence confidence);
    void Fallback(Connection& connection, U64 utc);
    void Flush(Connection& connection);
    void CloseWindows(const Traffic::ConnectionKey& key, U64 counter);
    bool Suppressed(const Traffic::NetworkEvent& event) const;
    bool SameOwner(const Connection& connection, const CTcpEStatsCollector::Observation& sample) const;

    std::map<U64, Connection> m_connections;
    std::map<Traffic::ConnectionKey, U64> m_aliases;
    std::map<Traffic::ConnectionKey, std::vector<Window>> m_windows;
    std::map<Traffic::ConnectionKey, Legacy> m_legacy;
    std::map<std::pair<U64, Traffic::Source>, Slice> m_slices;
    U64 m_generation{};
    unsigned m_windowCount{};
    bool m_healthy{};
    bool m_legacyHealthy{};
    ULONG m_legacyClock{};
    Diagnostics m_diagnostics;
};
