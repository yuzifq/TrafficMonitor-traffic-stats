#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <map>
#include <vector>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <filesystem>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>
#include <unordered_map>
#include "TrafficSnapshotFixture.h"
#define private public
#include "../ProcessTrafficPlugin/HybridTrafficCollector.h"
#include "../ProcessTrafficPlugin/TcpProofCollector.h"
#undef private
#include "../ProcessTrafficPlugin/TcpEStatsCollector.cpp"
#include "../ProcessTrafficPlugin/EtwProcNetCollector.cpp"
#include "../ProcessTrafficPlugin/HybridTrafficCollector.cpp"
#include "../ProcessTrafficPlugin/TcpProofCollector.cpp"
#include "../ProcessTrafficPlugin/TcpSourceTracker.cpp"

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void CounterChecks()
{
    CTcpEStatsCollector::State state;
    CTcpEStatsCollector::Diagnostics diagnostics;
    const auto apply = [&](Traffic::U64 rx, Traffic::U64 tx, DWORD error = NO_ERROR, bool enabled = true) {
        const auto at = state.value.end + 100;
        CTcpEStatsCollector::ApplyCounters(state, rx, tx, at, at + 1, error, enabled, diagnostics);
    };
    apply(1000000, 2000000);
    Check(state.value.baseline && !state.value.rxDelta && !state.value.txDelta, "First EStats sample counted prior bytes");
    apply(1000123, 2000456);
    Check(!state.value.baseline && state.value.rxDelta == 123 && state.value.txDelta == 456, "EStats increments not exact");
    Check(state.value.start <= state.value.startLatest && state.value.startLatest <= state.value.endEarliest &&
        state.value.endEarliest <= state.value.end, "Counter snapshot uncertainty brackets were presented as exact instants");
    apply(1000123, 2000456);
    Check(!state.value.rxDelta && !state.value.txDelta, "Idle snapshot repeated increments");
    apply(5, 2000457);
    Check(state.value.baseline && !state.value.txDelta && diagnostics.resets == 1, "Counter rollback failed to rebase both directions");
    apply(7, 2000459);
    Check(state.value.rxDelta == 2 && state.value.txDelta == 2, "New baseline lost subsequent bytes");
    state.value.cursor.sendMaximum = 12345; state.value.cursor.acknowledged = 67890;
    apply(0, 0, ERROR_ACCESS_DENIED, false);
    Check(!state.hasBaseline && !state.value.rxDelta && diagnostics.readFailures == 1 &&
        !state.value.cursor.sendMaximum && !state.value.cursor.acknowledged,
        "Failed sample kept an unsafe baseline or reused stale TCP sequence metadata");
    apply(9000000, 9000000);
    Check(state.value.baseline && !state.value.rxDelta, "Recovery backfilled an unmeasured failure interval");
    apply(9000007, 9000009);
    Check(state.value.rxDelta == 7 && state.value.txDelta == 9, "Recovery delta incorrect");
    CTcpEStatsCollector::State large;
    CTcpEStatsCollector::ApplyCounters(large, 1ULL << 40, 1ULL << 41, 1, 2, NO_ERROR, true, diagnostics);
    CTcpEStatsCollector::ApplyCounters(large, (1ULL << 40) + 9000000000ULL, (1ULL << 41) + 8000000000ULL,
        3, 4, NO_ERROR, true, diagnostics);
    Check(large.value.rxDelta == 9000000000ULL && large.value.txDelta == 8000000000ULL, "EStats counters truncated to 32 bits");
    CTcpEStatsCollector::ApplyCounters(large, 1ULL << 42, 1ULL << 43, 0, 1, NO_ERROR, true, diagnostics);
    Check(large.value.baseline && !large.value.rxDelta && !large.value.txDelta, "Clock rollback was treated as a valid measured interval");
    CTcpEStatsCollector::State cursorState;
    CTcpEStatsCollector::Cursor cursor;
    cursor.acknowledged = 100; cursor.received = 200;
    CTcpEStatsCollector::ApplyCounters(cursorState, 500, 600, 1, 2, NO_ERROR, true, diagnostics, &cursor);
    cursor.acknowledged = 110; cursor.received = 210;
    CTcpEStatsCollector::ApplyCounters(cursorState, 510, 610, 3, 4, NO_ERROR, true, diagnostics, &cursor);
    Check(!cursorState.value.baseline && cursorState.value.rxDelta == 10, "Valid sequence cursor reset the data baseline");
    cursor.acknowledged = 1;
    CTcpEStatsCollector::ApplyCounters(cursorState, 520, 620, 5, 6, NO_ERROR, true, diagnostics, &cursor);
    Check(cursorState.value.baseline && !cursorState.value.rxDelta && !cursorState.value.txDelta,
        "Collection-epoch cursor rollback reused the old counter baseline");
    std::cout << "PASS: EStats exact deltas, first baseline, idle, 64-bit counts, rollback and denied/recovered sampling\n";
}
void KeyChecks()
{
    CEtwProcNetCollector clock;
    clock.m_startFileTime = 134000000000000000ULL;
    clock.m_startCounter = 80000000;
    clock.m_counterFrequency = 10000000;
    clock.m_timestampClock = 1;
    bool before{};
    Check(clock.EventTime(80000001, before) == clock.m_startFileTime + 1 && !before,
        "Current QPC event was rejected against another clock");
    Check(clock.EventTime(79999999, before) == clock.m_startFileTime - 1 && before,
        "Buffered pre-start QPC event entered the ledger");
    Check(clock.EventTime(95000000, before) == clock.m_startFileTime + 15000000 && !before,
        "QPC elapsed time lost fractional seconds");
    clock.m_timestampClock = 2;
    Check(clock.EventTime(clock.m_startFileTime, before) == clock.m_startFileTime && !before,
        "System-time session changed its timestamps");
    Traffic::ConnectionKey a;
    a.pid = 1; a.family = 6; a.protocol = 6; a.localPort = 42; a.remotePort = 53;
    auto b = a;
    Check(a == b, "Equal connection keys differ");
    b.localScope = 2;
    Check(!(a == b), "IPv6 scope was omitted from identity");
    b = a; b.pid = 2;
    Check(!(a == b), "PID was omitted from identity");
    b = a; b.remote[15] = 2;
    Check(!(a == b), "Peer address omitted from identity");
    a.local[15] = 1;
    Check(Traffic::IsLoopback(a.local, 6), "IPv6 loopback not recognized");
    a.local = {}; a.local[10] = a.local[11] = 255; a.local[12] = 127;
    Check(Traffic::IsLoopback(a.local, 6), "Mapped IPv4 loopback not recognized");
    a.local[12] = 192;
    Check(!Traffic::IsLoopback(a.local, 6), "External mapped address incorrectly excluded");
    std::cout << "PASS: endpoint, PID, family/protocol and IPv6 scope identities, loopback parity\n";
}
void LifetimeChecks()
{
    CTcpEStatsCollector collector;
    Traffic::ConnectionKey key;
    key.pid = GetCurrentProcessId(); key.protocol = 6; key.family = 4;
    key.local = { 192, 0, 2, 1 }; key.remote = { 203, 0, 113, 1 };
    key.localPort = 1234; key.remotePort = 4321;
    collector.Visit(key, 100, true);
    const auto first = collector.m_connections.at(key).value.generation;
    collector.Visit(key, 200, true);
    const auto& second = collector.m_connections.at(key);
    Check(second.value.generation != first && !second.hasBaseline &&
        collector.m_observations.size() >= 3 && collector.m_observations[1].closed,
        "Reused TCP tuple inherited an old generation or baseline");
    const auto previousRead = second.value.end;
    collector.Visit(key, 200, true);
    Check(collector.m_connections.at(key).value.end == previousRead, "Failed EStats requests ignored retry backoff");
    CHybridTrafficCollector engine;
    Traffic::NetworkEvent event;
    event.key = key; event.bytes = 101; event.send = true;
    const auto owner = CTcpEStatsCollector::QueryOwner(key.pid);
    event.timestamp = owner.created - 1;
    engine.OnEvent(event);
    auto pending = std::move(engine.m_raw);
    engine.ProcessRaw(pending, Traffic::FileTimeNow());
    auto batch = engine.PeekPending();
    Check(batch.deltas.size() == 1 && batch.deltas[0].path.empty() && batch.deltas[0].confidence == Traffic::Confidence::Unknown &&
        batch.deltas[0].tx == 101, "A delayed event was assigned to a reused PID's current program");
    engine.Acknowledge(batch.sequence);
    event.timestamp = Traffic::FileTimeNow(); event.bytes = 500; event.connectionId = 1;
    engine.OnEvent(event);
    event.timestamp -= 1000; event.bytes = 700; event.connectionId = 2;
    engine.OnEvent(event);
    pending = std::move(engine.m_raw); engine.ProcessRaw(pending, Traffic::FileTimeNow());
    batch = engine.PeekPending();
    Check(batch.deltas.size() == 1 && batch.deltas[0].tx == 1200 && batch.deltas[0].path.empty() &&
        batch.deltas[0].start <= batch.deltas[0].end && batch.deltas[0].confidence == Traffic::Confidence::Unknown,
        "Mixed connection instances were guessed or out-of-order timestamps inverted the observation range");
    std::cout << "PASS: tuple generation, sampling backoff, reused PID rejection and delayed/mixed-instance event retention\n";
}
void Live(const wchar_t* path)
{
    std::ofstream file(path);
    if (!file) throw std::runtime_error("Cannot create EStats diagnostic report");
    CTcpEStatsCollector collector;
    Traffic::U64 rx{}, tx{}, valid{}, baselines{}, errors{};
    for (int i = 0; i < 12; ++i)
    {
        for (const auto& row : collector.Poll(i % 2 == 0))
        {
            if (row.closed) continue;
            if (row.error) ++errors;
            else if (row.enabled)
            {
                if (row.baseline) ++baselines;
                else { ++valid; rx += row.rxDelta; tx += row.txDelta; }
            }
        }
        const auto d = collector.GetDiagnostics();
        file << i << '\t' << d.connections << '\t' << d.enabled << '\t' << d.tableError4 << '\t' << d.tableError6
            << '\t' << d.lastError << '\t' << rx << '\t' << tx << '\n';
        file.flush();
        Sleep(500);
    }
    const auto d = collector.GetDiagnostics();
    file << "summary\tvalid\t" << valid << "\tbaselines\t" << baselines << "\terrors\t" << errors
        << "\tresets\t" << d.resets << "\tenableFailures\t" << d.enableFailures << "\tpollMicros\t" << d.pollMicros << '\n';
    collector.Stop();
    file << "done\n";
}

void LedgerChecks()
{
    CHybridTrafficCollector engine;
    Traffic::NetworkEvent event;
    event.key.pid = GetCurrentProcessId(); event.key.family = 4; event.key.protocol = 6;
    event.key.local[0] = 192; event.key.remote[0] = 10;
    event.key.localPort = 32; event.key.remotePort = 80;
    event.timestamp = Traffic::FileTimeNow(); event.bytes = 300; event.send = true; event.hasEndpoints = true;
    engine.OnEvent(event);
    event.send = false; event.bytes = 400;
    engine.OnEvent(event);
    auto raw = std::move(engine.m_raw);
    engine.ProcessRaw(raw, Traffic::FileTimeNow());
    engine.Publish(Traffic::FileTimeNow());
    const auto first = engine.PeekPending();
    Check(first.sequence && first.deltas.size() == 1 && first.deltas[0].rx == 400 && first.deltas[0].tx == 300,
        "Unified ledger did not preserve exact ETW deltas");
    for (int i = 0; i < 100; ++i) engine.GetSnapshot();
    Check(engine.PeekPending().sequence == first.sequence, "UI reads consumed the pending batch");
    auto owner = CTcpEStatsCollector::QueryOwner(event.key.pid);
    CTcpEStatsCollector::Observation shadow;
    shadow.key = event.key; shadow.owner = owner; shadow.connectionCreated = owner.created + 1;
    shadow.enabled = true; shadow.rxDelta = 9000000; shadow.txDelta = 8000000;
    engine.m_sources.Observe(shadow); engine.DrainSources();
    Check(engine.PeekPending().deltas[0].rx == 400 && !engine.GetDiagnostics().sources.switches,
        "Unqualified EStats bytes contaminated the official ledger");
    event.bytes = 500; event.send = true;
    engine.OnEvent(event); raw = std::move(engine.m_raw); engine.ProcessRaw(raw, Traffic::FileTimeNow());
    engine.Acknowledge(first.sequence + 1);
    Check(engine.PeekPending().sequence == first.sequence, "An invalid acknowledgment discarded pending traffic");
    engine.Acknowledge(first.sequence);
    const auto second = engine.PeekPending();
    Check(second.sequence != first.sequence && second.deltas.size() == 1 && second.deltas[0].tx == 500 && !second.deltas[0].rx,
        "History retries repeated an old batch or lost traffic collected during the retry");
    engine.Acknowledge(first.sequence);
    Check(engine.PeekPending().sequence == second.sequence, "A stale acknowledgment discarded a newer batch");
    engine.Acknowledge(second.sequence);
    Check(!engine.PeekPending().sequence, "Committed batch was replayed");
    shadow.error = ERROR_NOT_FOUND;
    engine.m_sources.Observe(shadow); engine.DrainSources();
    Check(!engine.PeekPending().sequence, "Failure of an unqualified source fabricated a traffic batch");
    auto other = owner;
    owner.path = L"C:\\one\\same.exe"; other.path = L"C:\\two\\same.exe";
    Check(engine.Identity(owner, 1) != engine.Identity(other, 1), "Same-name different-path applications were merged");
    other.path = L"C:\\ONE\\SAME.EXE";
    Check(engine.Identity(owner, 1) == engine.Identity(other, 1), "Windows path case created a duplicate application");
    std::cout << "PASS: exact ledger, immutable retries/ACK, nonconsuming UI, EStats isolation and path identity\n";
}

void MixedSourceBatchChecks()
{
    CHybridTrafficCollector engine;
    CHybridTrafficCollector::Delta a;
    a.identity = L"path:source-test.exe"; a.name = L"source-test.exe";
    a.path = L"C:\\test\\source-test.exe"; a.rx = 100; a.tx = 10; a.start = 1000; a.end = 1100;
    engine.Accumulate(a);
    auto b = a;
    b.source = Traffic::Source::EStats; b.rx = 200; b.tx = 20; b.start = 1100; b.end = 1200;
    b.confidence = Traffic::Confidence::Incomplete;
    engine.Accumulate(b);
    engine.Publish(1200);
    const auto batch = engine.PeekPending();
    Check(batch.deltas.size() == 2 && batch.deltas[0].source == Traffic::Source::Etw &&
        batch.deltas[0].rx == 100 && batch.deltas[0].tx == 10 && batch.deltas[0].end == 1100 &&
        batch.deltas[1].source == Traffic::Source::EStats && batch.deltas[1].rx == 200 &&
        batch.deltas[1].tx == 20 && batch.deltas[1].start == 1100,
        "Mixed-source history batch erased source provenance or combined sample intervals");
    const auto snapshots = engine.GetSnapshot();
    Check(snapshots.size() == 1 && snapshots[0].rx == 300 && snapshots[0].tx == 30 && snapshots[0].sources == 3 &&
        snapshots[0].confidence == Traffic::Confidence::Incomplete,
        "Mixed sources produced duplicate ranking rows or lost source/confidence metadata");
    b.rx = 7; b.tx = 9;
    engine.Accumulate(b);
    Check(engine.PeekPending().sequence == batch.sequence && engine.PeekPending().deltas[1].rx == 200,
        "New source increment mutated the immutable in-flight batch");
    engine.Acknowledge(batch.sequence);
    const auto next = engine.PeekPending();
    Check(next.sequence != batch.sequence && next.deltas.size() == 1 && next.deltas[0].source == Traffic::Source::EStats &&
        next.deltas[0].rx == 7 && next.deltas[0].tx == 9,
        "Mixed-source acknowledgment repeated committed bytes or discarded a new source increment");
    std::cout << "PASS: source-separated immutable history batches, merged app ranking and mixed-source confidence\n";
}

void LiveHybrid(const wchar_t* path)
{
    std::ofstream out(path);
    CHybridTrafficCollector collector;
    collector.Start();
    Traffic::U64 rx{}, tx{};
    for (int i = 0; i < 12; ++i)
    {
        Sleep(1000);
        const auto batch = collector.PeekPending();
        for (const auto& delta : batch.deltas) { rx += delta.rx; tx += delta.tx; }
        collector.Acknowledge(batch.sequence);
        const auto d = collector.GetDiagnostics();
        out << i << '\t' << rx << '\t' << tx << '\t' << d.estats.connections << '\t' << d.estats.enabled
            << '\t' << d.probing << '\t' << d.sources.estatsRx << '\t' << d.sources.estatsTx << '\t' << d.etw.eventsLost
            << '\t' << d.etw.parseFailures << '\t' << d.etw.endpointFailures << '\n';
        const auto status = collector.GetStatusText();
        out << "status\t";
        for (const auto c : status) out << static_cast<char>(c < 128 ? c : '?');
        out << '\t' << d.etw.queryError << '\t' << d.etw.sessionFlags << '\n';
        out.flush();
    }
    collector.Stop();
    out << "done\n";
}
void SchedulerChecks()
{
    struct CommitContext
    {
        CHybridTrafficCollector* collector;
        std::atomic<unsigned> calls{}, committed{};
        Traffic::U64 retrySequence{};
    };
    CHybridTrafficCollector collector;
    CommitContext context{ &collector };
    Check(collector.SetCommitSink([](void* data) {
        auto& state = *static_cast<CommitContext*>(data);
        ++state.calls;
        const auto batch = state.collector->PeekPending();
        if (!batch.sequence) return;
        for (const auto& delta : batch.deltas)
            if (delta.name == L"PID4294967294")
            {
                if (!state.retrySequence)
                {
                    state.retrySequence = batch.sequence;
                    throw std::runtime_error("Injected commit sink failure");
                }
                Check(batch.sequence == state.retrySequence && delta.rx == 123 && delta.tx == 456,
                    "Periodic retry changed or repeated an immutable batch");
                ++state.committed;
            }
        state.collector->Acknowledge(batch.sequence);
    }, &context), "Cannot set the history scheduler before startup");
    Check(collector.Start(), "Cannot start independent commit scheduler");
    Check(!collector.SetCommitSink(nullptr, nullptr), "A running sink was changed concurrently");
    Traffic::NetworkEvent event;
    event.key.pid = 4294967294u; event.key.protocol = 17; event.key.family = 4;
    event.timestamp = Traffic::FileTimeNow(); event.bytes = 123;
    collector.OnEvent(event);
    event.send = true; event.bytes = 456;
    collector.OnEvent(event);
    for (unsigned i = 0; i < 70 && context.committed.load() == 0; ++i)
    {
        collector.GetSnapshot();
        Sleep(100);
    }
    collector.Stop();
    Check(context.calls >= 2 && context.committed == 1 && collector.GetDiagnostics().commitFailures == 1,
        "Collection/checkpoint publication depended on host/UI calls or silently stopped after a commit exception");
    Check(collector.SetCommitSink(nullptr, nullptr), "Cannot clear sink after stop");
    std::cout << "PASS: independent periodic commit, nonconsuming UI reads, failed sink retry and stop lifecycle\n";
}
}
#include "TcpPathProbe.h"
#include "NativeTcpChecks.cpp"
#include "NativeUdpChecks.cpp"
#include "NativeStepChecks.cpp"
#include "NativeMixedChecks.cpp"
#include "NativeLoadChecks.cpp"
#include "ExternalTcpChecks.cpp"
#include "ProofSessionChecks.cpp"
#include "NativeEndpointChecks.cpp"
#include "TcpProofParserChecks.cpp"
#include "TcpSourceChecks.cpp"
#include "NativeSourceChecks.cpp"
#include "NaturalTrafficAudit.cpp"
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 4 && std::wstring(argv[1]) == L"--probe-server") return ProbeServer(std::wstring(argv[2]) == L"6", std::wstring(argv[3]) == L"short", std::wstring(argv[3]) == L"steps");
        if (argc == 3 && std::wstring(argv[1]) == L"--steps") { NativeStepProbe(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--sources") { NativeSourceProbe(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--hybrid-short") { NativeHybridShortProbe(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--endpoints") { NativeEndpointProbe(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--mixed") { NativeMixedProbe(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--native") { NativeProbe(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--short") { NativeShortProbe(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--udp") { NativeUdpProbe(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--load") { NativeLoad(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--external") { ExternalProbe(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--external-capture") { ExternalProbe(argv[2], true); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--live") { Live(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--live-hybrid") { LiveHybrid(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--audit-live") { NaturalTrafficAudit(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--audit-capture") { NaturalTrafficAudit(argv[2], true); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--audit-counters") { OfficialCountersAudit(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--audit-raw") { RawTrafficAudit(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--audit-raw-capture") { RawTrafficAudit(argv[2], true); return 0; }
        CounterChecks(); KeyChecks(); LifetimeChecks(); LedgerChecks(); ExactConnectionMatchChecks(); MixedSourceBatchChecks(); TcpProofParserChecks(); TcpSourceChecks(); SchedulerChecks(); return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
