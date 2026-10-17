namespace
{
struct SourceNativeSink
{
    CHybridTrafficCollector* engine{};
    std::atomic<bool> drop{};
    std::atomic<Traffic::U64> omittedRx{}, omittedTx{};
    static void Receive(void* context, const Traffic::NetworkEvent& event)
    {
        auto& sink = *static_cast<SourceNativeSink*>(context);
        if (event.key.pid != GetCurrentProcessId()) return;
        if (sink.drop && event.bytes && !event.retransmit)
        {
            (event.send ? sink.omittedTx : sink.omittedRx).fetch_add(event.bytes);
            return;
        }
        sink.engine->OnEvent(event);
    }
};
void NativeSourceProbe(const wchar_t* output)
{
    std::ofstream out(output);
    WSADATA wsa{};
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
    try
    {
        for (const bool failCounters : { false, true })
        for (const bool ipv6 : { false, true })
        {
            CHybridTrafficCollector engine;
            SourceNativeSink sink{ &engine };
            engine.m_etw.m_includeLoopback = true;
            engine.m_estats.m_includeLoopback = true;
            engine.m_includeProofLoopback = true;
            engine.m_etw.SetEventSink(SourceNativeSink::Receive, &sink);
            engine.Start(); Sleep(1500);
            ProbeChild peer(ipv6, L"steps");
            SocketGuard client;
            ConnectProbe(client, ipv6, peer.port);
            // A real nonzero prefix must stay with legacy ETW. It cannot be
            // replayed when Data/Thru counters begin at different epochs.
            TransferSend(client.value, probeSteps[0], 0x5a);
            TransferReceive(client.value, probeSteps[0], 0x6b);
            bool active{};
            for (unsigned i = 0; i < 100; ++i)
            {
                Sleep(100);
                const auto d = engine.GetDiagnostics();
                if (d.sources.active) { active = true; break; }
            }
            const auto report = [&](const char* phase) {
                const auto d = engine.GetDiagnostics();
                out << phase << '\t' << (ipv6 ? 6 : 4) << "\tactive\t" << d.sources.active << "\tswitches\t" << d.sources.switches
                    << "\tEStats\t" << d.sources.estatsRx << '\t' << d.sources.estatsTx << "\trejected\t" << d.sources.rejected
                    << "\tfallbacks\t" << d.sources.fallbacks << "\tproof\t" << d.proof.error << '\t' << d.proof.parseFailures
                    << "\tlegacy\t" << d.etw.clockType << '\t' << d.etw.parseFailures << '\n' << std::flush;
                if (!active)
                {
                    std::lock_guard<std::mutex> lock(engine.m_mutex);
                    for (const auto& entry : engine.m_sources.m_connections)
                    {
                        const auto& c = entry.second;
                        if (c.key.pid != GetCurrentProcessId()) continue;
                        out << "candidate\t" << c.key.pid << '\t' << c.connected << '\t' << c.haveSend << '\t' << c.haveReceive
                            << '\t' << c.eligible << '\t' << c.tx.seenPrefix << '\t' << c.rx.seenPrefix << '\t' << c.probeEnd
                            << '\t' << c.owner.created << '\t' << c.bornUtc << '\n';
                        const auto legacy = engine.m_sources.m_legacy.find(c.key);
                        if (legacy != engine.m_sources.m_legacy.end()) out << "legacy-prefix\t" << legacy->second.begin << '\t'
                            << c.begin << '\t' << legacy->second.rx << '\t' << legacy->second.tx << '\n';
                        const auto data = ReadProbeCounters(c.key);
                        out << "native-cursor\t" << data.DataBytesIn << '\t' << data.DataBytesOut << '\t' << data.ThruBytesReceived
                            << '\t' << data.ThruBytesAcked << '\t' << data.SndUna << '\t' << data.SndNxt << '\t' << data.SndMax
                            << '\t' << data.RcvNxt << "\torigin\t" << c.sendOrigin << '\t' << c.receiveOrigin
                            << "\thealth\t" << engine.m_sources.m_healthy << '\t' << engine.m_sources.m_legacyHealthy
                            << "\tstate\t" << static_cast<unsigned>(c.state) << "\tcontext\t" << c.contextCreated << '\n';
                    }
                    out.flush();
                }
            };
            report("qualified");
            Check(active, "Native busy-prefix connection never qualified for actual source handoff");
            sink.drop = true;
            Traffic::U64 expected = probeSteps[0];
            for (unsigned i = 1; i < std::size(probeSteps); ++i)
            {
                TransferSend(client.value, probeSteps[i], 0x5a);
                TransferReceive(client.value, probeSteps[i], 0x6b);
                expected += probeSteps[i];
                if (i == 3)
                {
                    Sleep(2500);
                    report("legacy-loss");
                    Check(engine.GetDiagnostics().sources.estatsRx == expected - 1 &&
                        engine.GetDiagnostics().sources.estatsTx == expected - 1,
                        "Independent EStats did not recover exactly the intentionally omitted legacy payload");
                    if (failCounters)
                    {
                        std::lock_guard<std::mutex> lock(engine.m_mutex);
                        for (const auto& entry : engine.m_sources.m_connections)
                        {
                            if (entry.second.key.pid != GetCurrentProcessId()) continue;
                            CTcpEStatsCollector::Observation failure;
                            failure.key = entry.second.key; failure.end = Traffic::FileTimeNow(); failure.error = ERROR_ACCESS_DENIED;
                            engine.m_sources.Observe(failure);
                        }
                        engine.DrainSources();
                    }
                }
            }
            Sleep(3500);
            engine.Stop();
            report("complete");
            const auto batch = engine.PeekPending();
            Traffic::U64 rx{}, tx{}, estatsRx{}, estatsTx{};
            for (const auto& delta : batch.deltas)
            {
                rx += delta.rx; tx += delta.tx;
                if (delta.source == Traffic::Source::EStats) { estatsRx += delta.rx; estatsTx += delta.tx; }
            }
            out << "amounts\t" << expected << '\t' << rx << '\t' << tx << "\testats\t" << estatsRx << '\t' << estatsTx
                << "\tomitted\t" << sink.omittedRx << '\t' << sink.omittedTx << '\n' << std::flush;
            Check(rx == expected && tx == expected && estatsRx && estatsTx && sink.omittedRx == expected - 1 && sink.omittedTx == expected - 1,
                "Native hybrid switch/failure fallback lost, replayed or double-counted known payload");
            if (!failCounters) Check(estatsRx == expected - 1 && estatsTx == expected - 1,
                "Healthy long-flow EStats source did not account the complete post-handoff payload");
        }
        out << "PASS: real IPv4/IPv6 nonzero handoff, omitted legacy recovery and exact EStats failure fallback\n";
    }
    catch (const std::exception& error) { out << "FAIL\t" << error.what() << '\n'; throw; }
    WSACleanup();
}
void NativeHybridShortProbe(const wchar_t* output)
{
    std::ofstream out(output);
    WSADATA wsa{};
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
    try
    {
        for (const bool ipv6 : { false, true })
        {
            CHybridTrafficCollector engine;
            SourceNativeSink sink{ &engine };
            engine.m_etw.m_includeLoopback = true;
            engine.m_estats.m_includeLoopback = true;
            engine.m_includeProofLoopback = true;
            engine.m_etw.SetEventSink(SourceNativeSink::Receive, &sink);
            engine.Start(); Sleep(1500);
            ProbeChild peer(ipv6, true);
            for (unsigned i = 0; i < 100; ++i)
            {
                SocketGuard client;
                ConnectProbe(client, ipv6, peer.port);
                TransferSend(client.value, 1024, 0x5a);
                TransferReceive(client.value, 1024, 0x6b);
            }
            Sleep(3000); engine.Stop();
            Traffic::U64 rx{}, tx{};
            for (const auto& delta : engine.PeekPending().deltas) { rx += delta.rx; tx += delta.tx; }
            const auto d = engine.GetDiagnostics();
            out << (ipv6 ? 6 : 4) << "\t100-short\t" << rx << '\t' << tx << "\tswitches\t" << d.sources.switches << '\n' << std::flush;
            Check(rx == 102400 && tx == 102400 && !d.etw.parseFailures && !d.proof.parseFailures,
                "Fully integrated hybrid engine lost or repeated short TCP payload");
        }
        out << "PASS: 100 IPv4 and 100 IPv6 short connections with both collectors scheduled\n";
    }
    catch (const std::exception& error) { out << "FAIL\t" << error.what() << '\n'; throw; }
    WSACleanup();
}
}
