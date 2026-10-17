#pragma once

#include <Windows.h>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "EtwProcNetCollector.h"
#include "TcpEStatsCollector.h"
#include "TcpSourceTracker.h"

class CHybridTrafficCollector
{
public:
    using CommitSink = void (*)(void*);
    struct Delta
    {
        std::wstring identity, name, path;
        Traffic::U64 rx{}, tx{}, start{}, end{};
        Traffic::Source source{ Traffic::Source::Etw };
        Traffic::Confidence confidence{ Traffic::Confidence::Normal };
    };
    struct AppSnapshot : Delta
    {
        Traffic::U64 rxPerSec{}, txPerSec{};
        unsigned sources{};
    };
    struct Batch
    {
        Traffic::U64 sequence{};
        std::vector<Delta> deltas;
    };
    struct Diagnostics
    {
        CEtwProcNetCollector::Diagnostics etw;
        CTcpEStatsCollector::Diagnostics estats;
        CTcpProofCollector::Diagnostics proof;
        CTcpSourceTracker::Diagnostics sources;
        Traffic::U64 unknownRx{}, unknownTx{}, queueOverflows{}, publications{}, commitFailures{};
        Traffic::U64 rawRx{}, rawTx{}, formalRx{}, formalTx{}, etwFormalRx{}, etwFormalTx{};
        Traffic::U64 captureUncertainIntervals{};
        std::uint32_t trackedConnections{}, probing{}, active{}, uncertain{};
        bool running{};
    };

    CHybridTrafficCollector();
    ~CHybridTrafficCollector();
    bool Start();
    void Stop();
    bool SetCommitSink(CommitSink sink, void* context);
    std::vector<AppSnapshot> GetSnapshot() const;
    Batch PeekPending();
    void Acknowledge(Traffic::U64 sequence);
    Diagnostics GetDiagnostics() const;
    std::wstring GetStatusText() const;

private:
    struct Raw
    {
        Traffic::NetworkEvent last;
        Traffic::U64 rx{}, tx{}, first{}, end{};
        bool mixedInstances{};
        bool opened{}, closed{};
    };
    struct Flow
    {
        Traffic::ProcessIdentity owner;
        Traffic::U64 connectionId{}, generation{}, first{}, lastTick{};
    };
    static void EventCallback(void* context, const Traffic::NetworkEvent& event);
    static void ProofCallback(void* context, const CTcpProofCollector::Event& event);
    void OnEvent(const Traffic::NetworkEvent& event);
    void Run();
    void ProcessRaw(std::map<Traffic::ConnectionKey, Raw>& raw, Traffic::U64 now);
    void Publish(Traffic::U64 now);
    void Accumulate(const Delta& delta);
    void RecordUnknown(Traffic::U64 start, Traffic::U64 end, Traffic::Confidence confidence);
    void RecordCaptureHealth(const CEtwProcNetCollector::Diagnostics& diagnostics, Traffic::U64 now, bool running);
    void DrainSources();
    static std::wstring Identity(const Traffic::ProcessIdentity& owner, Traffic::U64 generation);

    CEtwProcNetCollector m_etw;
    CTcpEStatsCollector m_estats;
    CTcpProofCollector m_proof;
    CTcpSourceTracker m_sources;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::mutex m_controlMutex;
    std::condition_variable m_wake;
    bool m_running{};
    std::map<Traffic::ConnectionKey, Raw> m_raw;
    std::map<Traffic::ConnectionKey, Flow> m_flows;
    using LedgerKey = std::pair<std::wstring, Traffic::Source>;
    std::map<std::wstring, AppSnapshot> m_current;
    std::map<LedgerKey, Delta> m_pending;
    Batch m_inflight;
    Traffic::U64 m_sequence{}, m_generation{}, m_lastPublish{}, m_lastPublishTick{};
    std::vector<AppSnapshot> m_snapshot;
    Diagnostics m_diagnostics;
    CommitSink m_commitSink{};
    void* m_commitContext{};
    bool m_includeProofLoopback{};
    bool m_captureIncomplete{};
};
