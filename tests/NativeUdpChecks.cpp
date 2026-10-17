namespace
{
void NativeUdpProbe(const wchar_t* output)
{
    std::ofstream out(output);
    WSADATA wsa{};
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
    try
    {
        for (const bool ipv6 : { false, true })
        {
            NativeCollector etw(GetCurrentProcessId(), 17);
            etw.m_includeLoopback = true;
            etw.Start(); Sleep(1200);
            Check(etw.m_running.load(), "UDP ETW consumer did not start");
            SocketGuard server, client;
            server.value = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            client.value = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            Check(server.value != INVALID_SOCKET && client.value != INVALID_SOCKET, "Cannot create UDP sockets");
            SocketTimeout(server.value); SocketTimeout(client.value);
            auto address = ProbeAddress(ipv6, 0);
            int length = ipv6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
            Check(bind(server.value, reinterpret_cast<sockaddr*>(&address), length) == 0, "UDP bind failed");
            Check(getsockname(server.value, reinterpret_cast<sockaddr*>(&address), &length) == 0, "UDP getsockname failed");
            Check(connect(client.value, reinterpret_cast<sockaddr*>(&address), length) == 0, "UDP connect failed");
            std::array<char, 16384> data{}, received{};
            Traffic::U64 expected{};
            unsigned packets{};
            for (const unsigned size : { 1u, 32u, 128u, 1024u, 4096u, 16384u })
                for (unsigned i = 0; i < 10; ++i)
                {
                    Check(send(client.value, data.data(), size, 0) == static_cast<int>(size), "UDP send failed");
                    Check(recv(server.value, received.data(), static_cast<int>(received.size()), 0) == static_cast<int>(size), "UDP payload size changed");
                    expected += size; ++packets;
                }
            Sleep(2200);
            const auto total = etw.GetProcessTrafficSnapshot()[GetCurrentProcessId()];
            const auto d = etw.GetDiagnostics();
            out << "UDP" << (ipv6 ? "6" : "4") << "\tpackets\t" << packets << "\tknownPayload\t" << expected
                << "\tETWRx\t" << total.rxTotalBytes << "\tETWTx\t" << total.txTotalBytes
                << "\tparseFailures\t" << d.parseFailures << "\teventsLost\t" << d.eventsLost << '\n' << std::flush;
            etw.Stop();
            Check(total.rxTotalBytes == expected && total.txTotalBytes == expected,
                "UDP ETW reported bytes differ from the verified payload; inspect protocol semantics");
        }
        out << "PASS\n";
    }
    catch (const std::exception& error) { out << "FAIL\t" << error.what() << '\n'; throw; }
    WSACleanup();
}
}
