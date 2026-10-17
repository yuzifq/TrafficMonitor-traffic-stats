namespace
{
void ExternalProbe(const wchar_t* prefix, bool capture = false)
{
    const std::filesystem::path root(prefix);
    std::ofstream out(root.wstring() + L".report");
    Check(static_cast<bool>(out), "Cannot create external TCP report");
    const auto finish = [&] { std::ofstream(root.wstring() + L".quit") << "done\n"; };
    try
    {
        DWORD pid{}; unsigned localPort{}, family{};
        for (unsigned i = 0; i < 200 && !pid; ++i)
        {
            std::ifstream input(root.wstring() + L".control");
            input >> pid >> localPort >> family;
            if (!pid) Sleep(100);
        }
        Check(pid && localPort && (family == 4 || family == 6), "TLS peer did not publish a connection identity");
        if (capture)
        {
            for (unsigned i = 0; i < 200 && !std::filesystem::exists(root.wstring() + L".capture-ready"); ++i) Sleep(100);
            Check(std::filesystem::exists(root.wstring() + L".capture-ready"), "Isolated packet capture did not become ready");
        }
        NativeCollector etw(pid);
        etw.Start(); Sleep(1500);
        Check(etw.m_running.load(), "External ETW consumer did not start");
        CTcpEStatsCollector estats;
        Traffic::ConnectionKey key;
        bool found{};
        for (const auto& row : estats.Poll())
            if (row.key.pid == pid && row.key.family == family && row.key.localPort == htons(static_cast<unsigned short>(localPort)))
            {
                key = row.key;
                Check(row.enabled && !row.error, "Cannot enable EStats on the external TLS flow");
                found = true;
            }
        Check(found, "TLS flow is absent from the native ownership table");
        NativePathProbe pathStats(key);
        Sleep(1800);
        estats.Poll();
        const auto rawBefore = ReadProbeCounters(key);
        const auto pathBefore = pathStats.Read();
        const auto baselineTime = Traffic::FileTimeNow();
        const auto before = etw.GetProcessTrafficSnapshot()[pid];
        const auto retransmitsBefore = etw.GetRetransmitBytes();
        std::ofstream(root.wstring() + L".go") << "go\n";
        std::uint64_t knownRx{}, knownTx{};
        unsigned status{};
        for (unsigned i = 0; i < 300 && !status; ++i)
        {
            std::ifstream input(root.wstring() + L".wire");
            input >> knownRx >> knownTx >> status;
            if (!status) Sleep(100);
        }
        Check(status == 200 && knownRx && knownTx, "Verified HTTPS test body was not returned");
        Sleep(2200);
        std::uint64_t rx{}, tx{};
        found = false;
        for (const auto& row : estats.Poll()) if (row.key == key && !row.closed)
        {
            Check(row.enabled && !row.error && !row.baseline, "External EStats snapshot lost its baseline");
            rx = row.rxDelta; tx = row.txDelta; found = true;
        }
        Check(found, "External TLS connection closed before the final snapshot");
        const auto after = etw.GetProcessTrafficSnapshot()[pid];
        const auto diagnostics = etw.GetDiagnostics();
        const auto rawAfter = ReadProbeCounters(key);
        const auto pathAfter = pathStats.Read();
        const auto retransmitted = static_cast<DWORD>(pathAfter.BytesRetrans - pathBefore.BytesRetrans);
        out << "path\tBytesRetrans\t" << retransmitted << "\tPktsRetrans\t" << pathAfter.PktsRetrans - pathBefore.PktsRetrans
            << "\tFastRetran\t" << pathAfter.FastRetran - pathBefore.FastRetran << "\tSacksRcvd\t" << pathAfter.SacksRcvd - pathBefore.SacksRcvd
            << "\tMSS\t" << pathAfter.CurMss << '\n';
        out << "times\t" << baselineTime << '\t' << Traffic::FileTimeNow() << '\n';
        const auto rawReport = [&](const char* label, const TCP_ESTATS_DATA_ROD_v0& value) {
            out << label << "\tDataIn\t" << value.DataBytesIn << "\tDataOut\t" << value.DataBytesOut
                << "\tAcked\t" << value.ThruBytesAcked << "\tReceived\t" << value.ThruBytesReceived
                << "\tDataSegsOut\t" << value.DataSegsOut << "\tSegsOut\t" << value.SegsOut
                << "\tSndUna\t" << value.SndUna << "\tSndNxt\t" << value.SndNxt << "\tSndMax\t" << value.SndMax << '\n';
        };
        rawReport("before", rawBefore); rawReport("after", rawAfter);
        for (const auto& example : etw.Examples())
            out << "event-sequence\t" << example.send << '\t' << example.sequence << '\t' << example.bytes << '\t' << example.connectionId << '\n';
        out << "family\t" << family << "\tknownTCPRx\t" << knownRx << "\tknownTCPTx\t" << knownTx
            << "\tETWRx\t" << after.rxTotalBytes - before.rxTotalBytes << "\tETWTx\t" << after.txTotalBytes - before.txTotalBytes
            << "\tEStatsRx\t" << rx << "\tEStatsTx\t" << tx << "\tEventsLost\t" << diagnostics.eventsLost
            << "\tETWRetransmitBytes\t" << etw.GetRetransmitBytes() - retransmitsBefore << '\n' << std::flush;
        pathStats.Stop(); estats.Stop(); etw.Stop(); finish();
        Check(after.rxTotalBytes - before.rxTotalBytes == knownRx && after.txTotalBytes - before.txTotalBytes == knownTx,
            "External ETW counts do not match the verified raw TCP stream");
        Check(rx == knownRx && tx == knownTx + retransmitted,
            "External EStats counts do not match the verified TCP stream plus measured retransmissions");
        out << "PASS\n";
    }
    catch (const std::exception& error) { finish(); out << "FAIL\t" << error.what() << '\n'; throw; }
}
}
