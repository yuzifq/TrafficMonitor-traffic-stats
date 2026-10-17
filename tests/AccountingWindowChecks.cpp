namespace
{
struct AccountingWindowSink
{
    CHybridTrafficCollector* engine{};
    std::atomic<Traffic::U64> begin{}, end{ UINT64_MAX }, rawRx{}, rawTx{}, tcpRx{}, tcpTx{}, udpRx{}, udpTx{};
    static void Receive(void* context, const Traffic::NetworkEvent& event)
    {
        auto& sink = *static_cast<AccountingWindowSink*>(context);
        if (event.preStart || event.retransmit || event.timestamp < sink.begin || event.timestamp >= sink.end) return;
        (event.send ? sink.rawTx : sink.rawRx).fetch_add(event.bytes);
        if (event.key.protocol == 6) (event.send ? sink.tcpTx : sink.tcpRx).fetch_add(event.bytes);
        if (event.key.protocol == 17) (event.send ? sink.udpTx : sink.udpRx).fetch_add(event.bytes);
        sink.engine->OnEvent(event);
    }
};

void AccountingWindowProbe(const wchar_t* output)
{
    const auto path = std::filesystem::absolute(output);
    const auto fixture = std::filesystem::path(path.wstring() + L".fixture");
    Check(!std::filesystem::exists(fixture), "Use a new accounting report path; fixtures are not overwritten");
    std::filesystem::create_directories(fixture);
    std::ofstream out(path);
    Check(static_cast<bool>(out), "Cannot write accounting report");
    CHistoryTrafficStore history;
    CNetworkTrafficStore network;
    CNetworkTrafficCollector adapter;
    history.Initialize(fixture.wstring()); network.Initialize(fixture.wstring());
    CHybridTrafficCollector engine;
    AccountingWindowSink sink{ &engine };
    sink.begin = UINT64_MAX;
    engine.m_etw.SetEventSink(AccountingWindowSink::Receive, &sink);
    Check(engine.Start(), "Cannot start accounting collectors");
    const auto readyDeadline = GetTickCount64() + 10000;
    while ((!engine.m_proof.GetDiagnostics().running || engine.m_etw.m_traceHandle == INVALID_PROCESSTRACE_HANDLE) &&
        GetTickCount64() < readyDeadline) Sleep(50);
    Check(engine.m_proof.GetDiagnostics().running && engine.m_etw.m_traceHandle != INVALID_PROCESSTRACE_HANDLE,
        "Accounting capture window cannot begin before both consumers are ready");
    adapter.Sample();
    Check(adapter.m_hasBaseline, "No connected adapter is available for accounting");
    adapter.SelectAdapter(adapter.m_previous.id);
    adapter.Sample();
    const auto begin = adapter.m_previousUtc;
    sink.begin = begin * 10000;
    CHistoryTrafficStore::TrafficAmount formal{}, estats{}, boundary{}, unknown{};
    const auto drain = [&] {
        const auto batch = engine.PeekPending();
        if (!batch.sequence) return;
        std::vector<CHistoryTrafficStore::AppTotalEntry> entries;
        CHistoryTrafficStore::TrafficAmount accepted{}, native{}, uncertain{}, crossing{};
        for (const auto& delta : batch.deltas)
        {
            if (delta.start < sink.begin || delta.end >= sink.end)
            {
                if (delta.end >= sink.begin && delta.start < sink.end)
                { crossing.rxBytes += delta.rx; crossing.txBytes += delta.tx; }
                continue;
            }
            CHistoryTrafficStore::AppTotalEntry entry{ delta.name, delta.path, delta.rx, delta.tx };
            entry.identity = delta.identity; entry.sampleStart = delta.start; entry.sampleEnd = delta.end;
            entry.source = delta.source; entry.confidence = delta.confidence;
            entries.push_back(std::move(entry));
            accepted.rxBytes += delta.rx; accepted.txBytes += delta.tx;
            if (delta.source == Traffic::Source::EStats) { native.rxBytes += delta.rx; native.txBytes += delta.tx; }
            if (delta.path.empty()) { uncertain.rxBytes += delta.rx; uncertain.txBytes += delta.tx; }
        }
        Check(history.Update(entries), "Temporary accounting history write failed");
        formal.rxBytes += accepted.rxBytes; formal.txBytes += accepted.txBytes;
        estats.rxBytes += native.rxBytes; estats.txBytes += native.txBytes;
        unknown.rxBytes += uncertain.rxBytes; unknown.txBytes += uncertain.txBytes;
        boundary.rxBytes += crossing.rxBytes; boundary.txBytes += crossing.txBytes;
        engine.Acknowledge(batch.sequence);
    };
    out << "begin_utc_ms\t" << begin << '\n';
    out << "epoch\tUTC FILETIME milliseconds since 1601-01-01\n";
    out << "adapter_id\t";
    for (const auto c : adapter.m_previous.id) out << static_cast<char>(c);
    out << '\n';
    const auto deadline = GetTickCount64() + 60000;
    while (GetTickCount64() < deadline)
    {
        Sleep(500);
        if (const auto interval = adapter.Sample()) Check(network.Add(*interval), "Temporary NIC history write failed");
        drain();
    }
    if (const auto interval = adapter.Sample()) Check(network.Add(*interval), "Final NIC sample write failed");
    const auto end = adapter.m_previousUtc;
    sink.end = end * 10000;
    // Delayed callbacks keep their event timestamps. Straddling EStats slices
    // remain explicit boundary amounts; no byte is prorated into this window.
    Sleep(2500);
    engine.Stop(); drain();
    Check(history.Checkpoint(true) && network.Flush(true), "Temporary accounting checkpoint failed");
    const auto nic = network.GetRangeUtc(begin, end);
    const auto stored = history.GetAllTimeTotal();
    CHistoryTrafficStore recovered;
    recovered.Initialize(fixture.wstring());
    const auto reopened = recovered.GetAllTimeTotal();
    const auto pair = [&](const char* label, Traffic::U64 rx, Traffic::U64 tx) { out << label << '\t' << rx << '\t' << tx << '\n'; };
    out << "end_utc_ms\t" << end << '\n';
    pair("raw_etw", sink.rawRx, sink.rawTx);
    pair("raw_tcp", sink.tcpRx, sink.tcpTx); pair("raw_udp", sink.udpRx, sink.udpTx);
    pair("estats_formal", estats.rxBytes, estats.txBytes);
    pair("hybrid_formal_in_window", formal.rxBytes, formal.txBytes);
    pair("hybrid_boundary_unallocated", boundary.rxBytes, boundary.txBytes);
    pair("hybrid_persisted", stored.rxBytes, stored.txBytes);
    pair("hybrid_reopened", reopened.rxBytes, reopened.txBytes);
    pair("nic_in_window", nic.amount.rxBytes, nic.amount.txBytes);
    pair("nic_boundary_unallocated", nic.boundary.rxBytes, nic.boundary.txBytes);
    pair("unknown_process_attribution", unknown.rxBytes, unknown.txBytes);
    out << "nic_covered_ms\t" << nic.coveredMs << "\trequested_ms\t" << nic.requestedMs << "\thealthy\t" << nic.healthy << '\n';
    const auto d = engine.GetDiagnostics();
    out << "health_session_including_drain\tetw_lost\t" << d.etw.eventsLost << "\tbuffer_lost\t" << d.etw.realtimeBuffersLost
        << "\tparse_failed\t" << d.etw.parseFailures << "\tproof_lost\t" << d.proof.eventsLost
        << "\tproof_error\t" << d.proof.error << "\tswitches\t" << d.sources.switches
        << "\tfallbacks\t" << d.sources.fallbacks << "\tcapacity\t" << d.sources.overflows << '\n';
    pair("session_ambiguous_withheld_not_proven_loss", d.sources.quarantinedRx, d.sources.quarantinedTx);
    out << "classification\tscope\tNIC covers the fixed adapter; process ETW covers all non-loopback interfaces.\n"
        << "classification\tproven_loss\tOnly native loss/error evidence is reported; byte differences are not loss estimates.\n"
        << "classification\tboundary\tWhole crossing intervals are unallocated, never prorated.\n"
        << "classification\tresidual\tNIC-minus-ETW and ETW-minus-hybrid differences require independent protocol/path evidence.\n";
    Check(stored.rxBytes == formal.rxBytes && stored.txBytes == formal.txBytes &&
        reopened.rxBytes == formal.rxBytes && reopened.txBytes == formal.txBytes,
        "History storage/restart differs from the exact in-window formal ledger");
    Check(d.proof.running || !d.proof.error, "Proof collection was unavailable during accounting");
    out << "PASS: identical bounds, explicit source/boundary/scope, exact history persistence; numerical equality with NIC is not asserted\n";
}
}
