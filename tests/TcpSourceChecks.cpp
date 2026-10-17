namespace
{
struct SourceFixture
{
    CTcpSourceTracker tracker;
    Traffic::ConnectionKey key;
    Traffic::ProcessIdentity owner;
    std::uint32_t txOrigin{ 1001 }, rxOrigin{ 2000 };
    SourceFixture(unsigned char family = 4)
    {
        key.pid = 42; key.family = family; key.protocol = 6;
        key.local = { 192, 0, 2, 1 }; key.remote = { 198, 51, 100, 2 }; key.localPort = 10; key.remotePort = 20;
        owner.pid = 42; owner.created = 1000; owner.path = L"C:\\test\\source.exe"; owner.name = L"source.exe";
        tracker.SetHealth(true, 1);
        Open();
    }
    void Proof(CTcpProofCollector::Kind kind, Traffic::U64 at, std::uint32_t sequence = 0, std::uint32_t bytes = 0,
        Traffic::U64 tcb = 1)
    {
        CTcpProofCollector::Event event;
        event.kind = kind; event.counter = at; event.timestamp = 2000 + at; event.tcb = tcb;
        event.key = key; event.endpoints = kind == CTcpProofCollector::Kind::Connect || kind == CTcpProofCollector::Kind::Established;
        event.sequence = sequence; event.bytes = bytes;
        tracker.OnProof(event);
    }
    bool Legacy(bool send, Traffic::U64 at, std::uint32_t bytes, bool connect = false)
    {
        Traffic::NetworkEvent event;
        event.key = key; event.clockType = 1; event.rawTimestamp = at; event.timestamp = 2000 + at;
        event.bytes = bytes; event.hasEndpoints = true; event.send = send; event.lifecycle = connect; event.opcode = connect ? 12 : 10;
        return tracker.KeepLegacy(event);
    }
    void Open()
    {
        Proof(CTcpProofCollector::Kind::Connect, 100);
        Proof(CTcpProofCollector::Kind::Syn, 110, txOrigin - 1);
        Proof(CTcpProofCollector::Kind::Receive, 120, rxOrigin, 1);
        Proof(CTcpProofCollector::Kind::Established, 130);
        Check(Legacy(false, 140, 0, true), "Legacy connect was suppressed before source qualification");
    }
    CTcpEStatsCollector::Observation Sample(Traffic::U64 at, Traffic::U64 rx = 0, Traffic::U64 tx = 0, bool baseline = false)
    {
        CTcpEStatsCollector::Observation sample;
        sample.key = key; sample.owner = owner; sample.generation = 1; sample.connectionCreated = 1500;
        sample.enabled = true; sample.baseline = baseline; sample.start = 2000 + at - 100; sample.end = 2000 + at;
        sample.readBeginCounter = at; sample.readEndCounter = at + 1;
        sample.rx = rx; sample.tx = tx; sample.cursor.received = rx; sample.cursor.acknowledged = tx;
        sample.cursor.sendUnacknowledged = sample.cursor.sendNext = sample.cursor.sendMaximum = txOrigin + static_cast<DWORD>(tx);
        sample.cursor.receiveNext = rxOrigin + static_cast<DWORD>(rx);
        return sample;
    }
    void Activate()
    {
        tracker.Observe(Sample(200, 0, 0, true)); tracker.Observe(Sample(400));
        Check(tracker.GetDiagnostics().active == 1 && tracker.GetDiagnostics().switches == 1,
            "Verified zero-prefix connection did not activate EStats");
    }
};
void TcpSourceChecks()
{
    for (const auto family : { 4, 6 })
    {
        SourceFixture f(static_cast<unsigned char>(family));
        f.Activate();
        Check(f.Legacy(false, 140, 0, true) && !f.Legacy(true, 450, 1),
            "A delayed duplicate connect reopened the switched legacy interval");
        Check(!f.Legacy(true, 500, 100) && !f.Legacy(false, 510, 200), "Active connection credited a second legacy source");
        f.Proof(CTcpProofCollector::Kind::Send, 500, f.txOrigin, 100);
        f.Proof(CTcpProofCollector::Kind::Receive, 510, f.rxOrigin, 200);
        auto sample = f.Sample(600, 200, 100);
        f.tracker.Observe(sample);
        auto slices = f.tracker.TakeSlices();
        Check(slices.size() == 1 && slices[0].source == Traffic::Source::EStats && slices[0].rx == 200 && slices[0].tx == 100,
            "Qualified EStats sample did not commit exact single-source increments");
        sample = f.Sample(800); sample.error = ERROR_ACCESS_DENIED; sample.enabled = false;
        f.tracker.Observe(sample);
        f.Proof(CTcpProofCollector::Kind::Send, 900, f.txOrigin + 50, 100);
        f.Proof(CTcpProofCollector::Kind::Send, 910, f.txOrigin + 100, 50);
        f.Proof(CTcpProofCollector::Kind::Receive, 920, f.rxOrigin + 200, 60);
        f.Proof(CTcpProofCollector::Kind::Fin, 930, f.rxOrigin + 261);
        slices = f.tracker.TakeSlices();
        Traffic::U64 rx{}, tx{};
        for (const auto& slice : slices)
        {
            Check(slice.source == Traffic::Source::Etw && slice.confidence == Traffic::Confidence::Incomplete,
                "Uncertain fallback lost its source/completeness provenance");
            rx += slice.rx; tx += slice.tx;
        }
        Check(rx == 60 && tx == 50 && f.tracker.GetDiagnostics().fallbacks == 1,
            "Fallback replayed the credited prefix, duplicated overlaps, or counted FIN as payload");
    }
    {
        SourceFixture f;
        f.Proof(CTcpProofCollector::Kind::Send, 150, f.txOrigin, 500);
        f.Proof(CTcpProofCollector::Kind::Receive, 160, f.rxOrigin, 400);
        Check(f.Legacy(true, 155, 500) && f.Legacy(false, 165, 400), "Pre-handoff legacy prefix was consumed");
        auto sample = f.Sample(200, 800, 900, true);
        sample.cursor.received = sample.cursor.acknowledged = 0;
        sample.cursor.receiveNext = f.rxOrigin + 400;
        sample.cursor.sendUnacknowledged = sample.cursor.sendNext = sample.cursor.sendMaximum = f.txOrigin + 500;
        f.tracker.Observe(sample);
        sample.baseline = false; sample.readBeginCounter = 400; sample.readEndCounter = 401;
        f.tracker.Observe(sample);
        Check(f.tracker.GetDiagnostics().active == 1 && f.tracker.TakeSlices().empty(),
            "Busy-prefix handoff failed or replayed traffic predating its baseline");
        sample.rx += 30; sample.tx += 20; sample.cursor.received = 30; sample.cursor.acknowledged = 20;
        sample.cursor.receiveNext += 30; sample.cursor.sendMaximum += 20;
        sample.cursor.sendUnacknowledged = sample.cursor.sendNext = sample.cursor.sendMaximum;
        sample.readBeginCounter = 600; sample.readEndCounter = 601;
        f.tracker.Observe(sample);
        const auto slices = f.tracker.TakeSlices();
        Check(slices.size() == 1 && slices[0].rx == 30 && slices[0].tx == 20,
            "Different native counter epochs fabricated an old prefix or lost post-handoff traffic");
    }
    {
        SourceFixture f;
        f.Activate();
        f.Proof(CTcpProofCollector::Kind::Send, 500, f.txOrigin, 60);
        auto sample = f.Sample(600, 0, 60); sample.tx = 100;
        f.tracker.Observe(sample);
        const auto slices = f.tracker.TakeSlices();
        Traffic::U64 tx{};
        for (const auto& slice : slices) { Check(slice.source == Traffic::Source::Etw, "Larger EStats value was selected as better"); tx += slice.tx; }
        Check(tx == 60 && f.tracker.GetDiagnostics().rejected == 1, "Disagreeing counter was added instead of exact ETW fallback");
    }
    {
        SourceFixture f;
        f.tracker.SetHealth(true, 1, false);
        f.tracker.Observe(f.Sample(200, 0, 0, true)); f.tracker.Observe(f.Sample(400));
        Check(!f.tracker.GetDiagnostics().active, "Incomplete initial legacy prefix qualified a connection");
        f.tracker.SetHealth(true, 1); f.Activate();
        f.tracker.SetHealth(true, 1, false);
        f.tracker.Observe(f.Sample(600, 50, 70));
        auto slices = f.tracker.TakeSlices();
        Check(slices.size() == 1 && slices[0].rx == 50 && slices[0].tx == 70,
            "Independent counters could not cover a lost legacy interval");
        f.tracker.SetHealth(false, 1);
        f.Proof(CTcpProofCollector::Kind::Send, 700, f.txOrigin + 70, 100);
        f.tracker.Observe(f.Sample(800, 50, 170));
        slices = f.tracker.TakeSlices();
        Check(slices.size() == 1 && !slices[0].rx && !slices[0].tx && slices[0].confidence == Traffic::Confidence::Incomplete,
            "Lost identity evidence silently produced formal bytes");
    }
    {
        SourceFixture f;
        f.owner.created = 999999;
        f.tracker.Observe(f.Sample(200, 0, 0, true)); f.tracker.Observe(f.Sample(400));
        Check(!f.tracker.GetDiagnostics().switches && f.Legacy(true, 500, 10), "Reused PID inherited an old connection source");
    }
    {
        SourceFixture f;
        f.Activate();
        f.Proof(CTcpProofCollector::Kind::Receive, 500, f.rxOrigin, 1);
        f.Proof(CTcpProofCollector::Kind::Fin, 510, f.rxOrigin + 1);
        Traffic::U64 bytes{};
        for (const auto& slice : f.tracker.TakeSlices()) bytes += slice.rx + slice.tx;
        Check(!bytes, "A TCP FIN sequence-space octet was counted as application data");
    }
    {
        SourceFixture f;
        f.Activate();
        Check(f.Legacy(false, 900, 0, true) && f.Legacy(true, 910, 123),
            "A new connection instance inherited the previous source suppression");
        Check(!f.Legacy(true, 500, 123) && !f.tracker.GetDiagnostics().active,
            "Late old-instance bytes escaped the closed suppression window");
    }
    {
        SourceFixture f;
        f.Activate();
        f.tracker.m_connections.at(1).readTick = GetTickCount64() - 3000;
        f.tracker.Expire();
        Check(!f.tracker.GetDiagnostics().active && f.tracker.GetDiagnostics().fallbacks == 1,
            "Missing connection-table observations left stale EStats accounting active");
    }
    {
        SourceFixture f;
        f.Activate();
        const Traffic::U64 large = (1ULL << 32) + 123;
        f.tracker.Observe(f.Sample(600, large, large + 456));
        auto slices = f.tracker.TakeSlices();
        Check(slices.size() == 1 && slices[0].rx == large && slices[0].tx == large + 456,
            "64-bit source increments were truncated at a TCP sequence wrap");
        auto failed = f.Sample(800); failed.error = ERROR_NOT_FOUND;
        f.tracker.Observe(failed);
        f.Proof(CTcpProofCollector::Kind::Send, 900, f.txOrigin + 123 + 456 + 100, 50);
        f.Proof(CTcpProofCollector::Kind::Send, 910, f.txOrigin + 123 + 456, 100);
        f.Proof(CTcpProofCollector::Kind::Send, 920, f.txOrigin + 123 + 456, 100);
        Traffic::U64 tx{};
        for (const auto& slice : f.tracker.TakeSlices()) tx += slice.tx;
        Check(tx == 150, "Out-of-order/duplicate sequence ranges were replayed after 32-bit wrap");
    }
    {
        SourceFixture f;
        f.Activate();
        f.Proof(CTcpProofCollector::Kind::Connect, 1000);
        f.Proof(CTcpProofCollector::Kind::Syn, 1010, f.txOrigin - 1);
        f.Proof(CTcpProofCollector::Kind::Receive, 1020, f.rxOrigin, 1);
        f.Proof(CTcpProofCollector::Kind::Established, 1030);
        f.Legacy(false, 1040, 0, true);
        f.tracker.Observe(f.Sample(1200, 0, 0, true)); f.tracker.Observe(f.Sample(1400));
        Check(f.tracker.GetDiagnostics().switches == 2 && f.tracker.GetDiagnostics().active == 1 &&
            !f.Legacy(true, 500, 10) && !f.Legacy(true, 1500, 20),
            "Reused tuple/TCB lost its generation or reopened an old legacy interval");
    }
    {
        SourceFixture f;
        f.Proof(CTcpProofCollector::Kind::Send, 150, f.txOrigin + 0x80000000u, 100);
        Check(f.tracker.GetDiagnostics().overflows == 1 && !f.tracker.GetDiagnostics().switches &&
            f.Legacy(true, 200, 4096) && !f.tracker.GetDiagnostics().suppressedTx,
            "Pre-handoff sequence rejection unexpectedly consumed the legacy byte source");
    }
    {
        SourceFixture f;
        f.Activate();
        f.Proof(CTcpProofCollector::Kind::Close, 500);
        const auto d = f.tracker.GetDiagnostics();
        Traffic::U64 bytes{};
        for (const auto& slice : f.tracker.TakeSlices()) bytes += slice.rx + slice.tx;
        Check(d.fallbacks == 1 && d.gaps == 1 && !bytes,
            "Idle normal close no longer matches the documented diagnostic counter interpretation");
    }
    std::cout << "PASS: real source activation, nonzero prefix/epoch handoff, overlap-clipped fallback, FIN, loss and reused PID gates\n";
    std::cout << "PASS: pre-handoff rejection preserves legacy bytes; normal idle close also increments fallback/gap diagnostics\n";
}
void ExactConnectionMatchChecks()
{
    for (const auto family : { 4, 6 })
    {
        SourceFixture f(static_cast<unsigned char>(family));
        f.Activate();
        auto wrong = f.Sample(600, 13, 17);
        std::swap(wrong.key.local, wrong.key.remote);
        std::swap(wrong.key.localPort, wrong.key.remotePort);
        f.tracker.Observe(wrong);
        Check(f.tracker.TakeSlices().empty() && f.tracker.GetDiagnostics().active == 1,
            "Opposite same-PID endpoints matched a verified connection");
        wrong = f.Sample(600, 13, 17); wrong.key.localScope = 7;
        f.tracker.Observe(wrong);
        Check(f.tracker.TakeSlices().empty(), "IPv6 scope identity was guessed");
        f.tracker.Observe(f.Sample(600, 13, 17));
        const auto slices = f.tracker.TakeSlices();
        Check(slices.size() == 1 && slices[0].key == f.key && slices[0].rx == 13 && slices[0].tx == 17,
            "Exact verified connection did not receive its native counters");
    }
    std::cout << "PASS: actual source tracker requires exact IPv4/IPv6 endpoints and scopes, no reversed same-PID fallback\n";
}
}
