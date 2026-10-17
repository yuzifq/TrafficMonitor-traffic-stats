namespace
{
void NativeMixedProbe(const wchar_t* output)
{
    std::ofstream out(output);
    WSADATA wsa{};
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
    try
    {
        for (const bool ipv6 : { false, true })
        {
            CHybridTrafficCollector collector;
            collector.m_etw.m_includeLoopback = true;
            collector.m_estats.m_includeLoopback = true;
            collector.m_includeProofLoopback = true;
            collector.Start(); Sleep(1200);
            Check(collector.m_etw.m_running.load(), "Mixed ETW consumer did not start");
            ProbeChild peer(ipv6, L"steps");
            SocketGuard tcp, udpSender, udpReceiver;
            ConnectProbe(tcp, ipv6, peer.port);
            udpSender.value = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            udpReceiver.value = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            Check(udpSender.value != INVALID_SOCKET && udpReceiver.value != INVALID_SOCKET, "Mixed UDP creation failed");
            SocketTimeout(udpSender.value); SocketTimeout(udpReceiver.value);
            auto address = ProbeAddress(ipv6, 0);
            int length = ipv6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
            Check(bind(udpReceiver.value, reinterpret_cast<sockaddr*>(&address), length) == 0 &&
                getsockname(udpReceiver.value, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
                connect(udpSender.value, reinterpret_cast<sockaddr*>(&address), length) == 0, "Mixed UDP bind/connect failed");
            const auto identity = collector.Identity(CTcpEStatsCollector::QueryOwner(GetCurrentProcessId()), 0);
            Traffic::U64 rx{}, tx{}, expected{};
            const auto drain = [&] {
                const auto batch = collector.PeekPending();
                for (const auto& delta : batch.deltas) if (delta.identity == identity)
                {
                    rx += delta.rx; tx += delta.tx;
                }
                collector.Acknowledge(batch.sequence);
            };
            std::exception_ptr udpError;
            std::thread udp([&] {
                try
                {
                    std::array<char, 4096> bytes{}, received{};
                    for (unsigned i = 0; i < 60; ++i)
                    {
                        const int size = i % 2 ? 127 : 4096;
                        Check(send(udpSender.value, bytes.data(), size, 0) == size &&
                            recv(udpReceiver.value, received.data(), static_cast<int>(received.size()), 0) == size,
                            "Mixed UDP lost its verified payload");
                        Sleep(10);
                    }
                }
                catch (...) { udpError = std::current_exception(); }
            });
            try
            {
                for (const auto size : probeSteps)
                {
                    TransferSend(tcp.value, size, 0x5a);
                    TransferReceive(tcp.value, size, 0x6b);
                    // The isolated TCP peer runs this same executable path;
                    // application identity therefore includes both endpoints.
                    expected += static_cast<Traffic::U64>(size) * 2;
                    drain(); Sleep(70);
                }
            }
            catch (...) { udp.join(); throw; }
            udp.join();
            if (udpError) std::rethrow_exception(udpError);
            expected += 30 * (4096 + 127);
            for (unsigned i = 0; i < 100 && (rx < expected || tx < expected); ++i) { Sleep(100); drain(); }
            collector.Stop(); drain();
            const auto diagnostics = collector.GetDiagnostics();
            out << (ipv6 ? 6 : 4) << "\texpectedPerDirection\t" << expected << "\tledgerRx\t" << rx << "\tledgerTx\t" << tx
                << "\testatsRx\t" << diagnostics.sources.estatsRx << "\testatsTx\t" << diagnostics.sources.estatsTx
                << "\tloss\t" << diagnostics.etw.eventsLost << "\tparseFailures\t" << diagnostics.etw.parseFailures << '\n' << std::flush;
            Check(rx == expected && tx == expected && !diagnostics.etw.eventsLost && !diagnostics.etw.parseFailures,
                "Concurrent native TCP/UDP workload was lost or counted twice");
        }
        out << "PASS\n";
    }
    catch (const std::exception& error) { out << "FAIL\t" << error.what() << '\n'; throw; }
    WSACleanup();
}
}
