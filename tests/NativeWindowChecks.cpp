namespace
{
void NativeWindowProbe(const wchar_t* output)
{
    std::ofstream out(output);
    Check(static_cast<bool>(out), "Cannot write native window lifecycle report");
    HANDLE token{};
    TOKEN_ELEVATION elevation{};
    DWORD size{};
    const bool tokenOpened = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE;
    const bool elevated = tokenOpened && GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
        elevation.TokenIsElevated;
    if (token) CloseHandle(token);
    out << "elevated\t" << elevated << '\n' << std::flush;
    Check(elevated, "Native ETW/EStats lifecycle verification requires an elevated process");
    WSADATA wsa{};
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
    try
    {
        CHybridTrafficCollector engine;
        SourceNativeSink sink{ &engine };
        engine.m_etw.m_includeLoopback = true;
        engine.m_estats.m_includeLoopback = true;
        engine.m_includeProofLoopback = true;
        engine.m_etw.SetEventSink(SourceNativeSink::Receive, &sink);
        Check(engine.Start(), "Cannot start native lifecycle collectors");
        Sleep(1500);
        const auto initial = engine.GetDiagnostics();
        out << "startup\tproof\t" << initial.proof.error << "\tlegacy\t" << initial.etw.queryError << '\n' << std::flush;
        Check(initial.proof.running && !initial.proof.error && engine.m_etw.m_running,
            "Native tracing sessions did not start; this is not a source-lifecycle test result");
        SocketGuard listener;
        listener.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        auto address = ProbeAddress(false, 0);
        int length = sizeof(sockaddr_in);
        Check(bind(listener.value, reinterpret_cast<sockaddr*>(&address), length) == 0 &&
            listen(listener.value, SOMAXCONN) == 0 &&
            getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "Cannot prepare native lifecycle listener");
        Traffic::U64 created{}, maximum{}, lastSwitches{};
        for (unsigned batch = 0; batch < 40; ++batch)
        {
            struct Pairs
            {
                std::vector<std::pair<SOCKET, SOCKET>> sockets;
                ~Pairs() { for (auto pair : sockets) { closesocket(pair.first); closesocket(pair.second); } }
            } pairs;
            for (unsigned i = 0; i < 256; ++i)
            {
                SocketGuard client;
                ConnectProbe(client, false, AddressPort(address, false));
                const auto peer = accept(listener.value, nullptr, nullptr);
                Check(peer != INVALID_SOCKET, "Cannot accept lifecycle connection");
                pairs.sockets.emplace_back(client.value, peer);
                client.value = INVALID_SOCKET;
                ++created;
            }
            const auto deadline = GetTickCount64() + 6500;
            CHybridTrafficCollector::Diagnostics d;
            do
            {
                Sleep(100);
                d = engine.GetDiagnostics();
            } while (d.sources.switches < lastSwitches + 256 && GetTickCount64() < deadline);
            lastSwitches = d.sources.switches;
            maximum = (std::max)(maximum, static_cast<Traffic::U64>(d.sources.windows));
            for (auto pair : pairs.sockets)
            {
                char byte = 'X', received{};
                Check(send(pair.first, &byte, 1, 0) == 1 && recv(pair.second, &received, 1, MSG_WAITALL) == 1 &&
                    received == byte && send(pair.second, &byte, 1, 0) == 1 &&
                    recv(pair.first, &received, 1, MSG_WAITALL) == 1 && received == byte,
                    "Native lifecycle payload transfer failed");
            }
            Sleep(1200);
            out << batch << "\tcreated\t" << created << "\tswitches\t" << d.sources.switches
                << "\twindows\t" << d.sources.windows << "\tretired\t" << d.sources.reclaimedWindows << '\n' << std::flush;
            std::cout << "native-lifecycle batch=" << batch << " created=" << created << " switches=" << d.sources.switches
                << " windows=" << d.sources.windows << " retired=" << d.sources.reclaimedWindows << '\n' << std::flush;
            if (created > 5000 && d.sources.switches > 5000) break;
        }
        Sleep(3500);
        const auto beforeStop = engine.GetDiagnostics();
        engine.Stop();
        Traffic::U64 rx{}, tx{};
        for (const auto& delta : engine.PeekPending().deltas) { rx += delta.rx; tx += delta.tx; }
        const auto d = engine.GetDiagnostics();
        out << "final\tcreated\t" << created << "\tswitches\t" << d.sources.switches << "\twindows\t"
            << beforeStop.sources.windows << "\tmaximum\t" << maximum << "\treclaimed\t" << d.sources.reclaimedWindows
            << "\trx\t" << rx << "\ttx\t" << tx << "\texpected\t" << created * 2
            << "\tlegacyLoss\t" << d.etw.eventsLost << "\tproofLoss\t" << d.proof.eventsLost
            << "\tcapacity\t" << d.sources.overflows << '\n' << std::flush;
        Check(created > 5000 && d.sources.switches > 5000, "Fewer than 5000 real connections qualified for handoff");
        Check(beforeStop.sources.windows < 256 && d.sources.reclaimedWindows > 5000 && !d.sources.overflows,
            "Real closed windows accumulated or exhausted capacity");
        Check(rx == created * 2 && tx == created * 2, "Native lifecycle test duplicated or lost exact payload bytes");
        Check(!d.etw.eventsLost && !d.etw.parseFailures && !d.proof.eventsLost && !d.proof.parseFailures,
            "Native lifecycle evidence was incomplete");
        out << "PASS: >5000 real TCP lifetimes and verified handoffs, bounded windows, exact payload\n";
    }
    catch (const std::exception& error) { out << "FAIL\t" << error.what() << '\n'; WSACleanup(); throw; }
    WSACleanup();
}
}
