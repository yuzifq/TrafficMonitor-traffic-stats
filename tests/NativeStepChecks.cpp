namespace
{
TCP_ESTATS_DATA_ROD_v0 ReadProbeCounters(const Traffic::ConnectionKey& key)
{
    TCP_ESTATS_DATA_ROD_v0 counters{};
    DWORD error;
    if (key.family == 6)
    {
        auto row = Row6(key);
        error = GetPerTcp6ConnectionEStats(&row, TcpConnectionEstatsData, nullptr, 0, 0, nullptr, 0, 0,
            reinterpret_cast<PUCHAR>(&counters), 0, sizeof(counters));
    }
    else
    {
        auto row = Row4(key);
        error = GetPerTcpConnectionEStats(&row, TcpConnectionEstatsData, nullptr, 0, 0, nullptr, 0, 0,
            reinterpret_cast<PUCHAR>(&counters), 0, sizeof(counters));
    }
    Check(error == NO_ERROR, "Cannot read raw step counters");
    return counters;
}
void NativeStepProbe(const wchar_t* output)
{
    std::ofstream out(output);
    WSADATA wsa{};
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
    try
    {
        for (const bool ipv6 : { false, true })
        {
            NativeCollector etw;
            etw.m_includeLoopback = true;
            etw.Start(); Sleep(1200);
            Check(etw.m_running.load(), "ETW step consumer did not start");
            ProbeChild peer(ipv6, L"steps");
            SocketGuard client;
            ConnectProbe(client, ipv6, peer.port);
            sockaddr_storage local{};
            int length = sizeof(local);
            Check(getsockname(client.value, reinterpret_cast<sockaddr*>(&local), &length) == 0, "Cannot read step client endpoint");
            const auto port = AddressPort(local, ipv6);
            CTcpEStatsCollector estats;
            estats.m_includeLoopback = true;
            Traffic::ConnectionKey key;
            bool found{};
            for (const auto& row : estats.Poll())
                if (row.key.pid == GetCurrentProcessId() && row.key.localPort == port && row.key.family == (ipv6 ? 6 : 4))
                {
                    Check(!row.error && row.enabled && row.baseline, "Step counter enable failed");
                    key = row.key; found = true;
                }
            Check(found, "Step flow not enumerated");
            Traffic::U64 expected{};
            const auto report = [&](unsigned size) {
                const auto data = ReadProbeCounters(key);
                bool observed{};
                for (const auto& sample : estats.Poll()) if (sample.key == key && !sample.closed)
                {
                    Check(sample.enabled && !sample.error && sample.rx == data.DataBytesIn && sample.tx == data.DataBytesOut,
                        "Quiescent native counters changed during cursor verification");
                    Check(sample.readBeginCounter && sample.readEndCounter >= sample.readBeginCounter,
                        "Native EStats snapshot lost its common QPC read brackets");
                    Check(sample.cursor.acknowledged == data.ThruBytesAcked && sample.cursor.received == data.ThruBytesReceived &&
                        sample.cursor.sendUnacknowledged == data.SndUna && sample.cursor.sendNext == data.SndNxt &&
                        sample.cursor.sendMaximum == data.SndMax && sample.cursor.receiveNext == data.RcvNxt,
                        "Collector lost TCP sequence metadata from the same EStats snapshot");
                    observed = true;
                }
                Check(observed, "Native sequence metadata observation missing");
                out << (ipv6 ? 6 : 4) << '\t' << size << '\t' << expected << '\t' << data.DataBytesIn << '\t'
                    << data.DataBytesOut << '\t' << data.ThruBytesAcked << '\t' << data.ThruBytesReceived << '\t'
                    << data.DataSegsOut << '\t' << data.SegsOut << '\t' << etw.GetRetransmitBytes() << '\n' << std::flush;
            };
            report(0); Sleep(1800); report(0);
            for (const auto size : probeSteps)
            {
                TransferSend(client.value, size, 0x5a);
                TransferReceive(client.value, size, 0x6b);
                expected += size;
                Sleep(300);
                report(size);
            }
            Sleep(2200);
            const auto total = etw.GetProcessTrafficSnapshot()[GetCurrentProcessId()];
            out << "ETW\t" << (ipv6 ? 6 : 4) << '\t' << expected << '\t' << total.rxTotalBytes << '\t' << total.txTotalBytes << '\n' << std::flush;
            Check(total.rxTotalBytes == expected && total.txTotalBytes == expected, "Step ETW payload mismatch");
            estats.Stop(); etw.Stop();
        }
        out << "PASS\n";
    }
    catch (const std::exception& error) { out << "FAIL\t" << error.what() << '\n'; throw; }
    WSACleanup();
}
}
