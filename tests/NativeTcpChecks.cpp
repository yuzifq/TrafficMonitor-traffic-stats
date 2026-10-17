// Included by HybridTrafficChecks.cpp so tests can exercise private collector
// boundaries without exposing injection or loopback switches in the DLL API.
namespace
{
constexpr unsigned probeUpload = 4 * 1024 * 1024;
constexpr unsigned probeDownload = 3 * 1024 * 1024;
constexpr unsigned probeSteps[] = { 1, 32, 128, 1024, 4096, 16384, 65536, 1048576, 4194304 };
TCP_ESTATS_DATA_ROD_v0 ReadProbeCounters(const Traffic::ConnectionKey& key);
class NativeCollector : public CEtwProcNetCollector
{
    mutable std::mutex mutex;
    ProcessTrafficSnapshot totals;
    std::uint64_t retransmits{};
    std::uint64_t rejectedRx{}, rejectedTx{}, rejectedTime{};
    std::vector<Traffic::NetworkEvent> examples;
    std::map<DWORD, ProcessTrafficSnapshot> protocolPids;
    std::uint16_t probePort{};
    DWORD pid;
    unsigned char protocol;
public:
    explicit NativeCollector(DWORD target = GetCurrentProcessId(), unsigned char targetProtocol = 6) : pid(target), protocol(targetProtocol)
    {
        m_captureSequence = true;
        m_reportRejected = true;
        SetEventSink([](void* context, const Traffic::NetworkEvent& event) {
            auto& collector = *static_cast<NativeCollector*>(context);
            if (event.lifecycle || event.key.protocol != collector.protocol) return;
            std::lock_guard<std::mutex> lock(collector.mutex);
            if (!event.preStart && !event.retransmit && (!collector.probePort ||
                event.key.localPort == collector.probePort || event.key.remotePort == collector.probePort))
            {
                auto& observed = collector.protocolPids[event.key.pid];
                (event.send ? observed.txTotalBytes : observed.rxTotalBytes) += event.bytes;
            }
            if (event.key.pid != collector.pid) return;
            if (event.preStart)
            {
                (event.send ? collector.rejectedTx : collector.rejectedRx) += event.bytes;
                collector.rejectedTime = event.timestamp;
                return;
            }
            if (event.retransmit) { collector.retransmits += event.bytes; return; }
            if (collector.examples.size() < 16 && event.hasSequence) collector.examples.push_back(event);
            (event.send ? collector.totals.txTotalBytes : collector.totals.rxTotalBytes) += event.bytes;
        }, this);
    }
    ~NativeCollector() { Stop(); }
    std::map<DWORD, ProcessTrafficSnapshot> GetProcessTrafficSnapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return { { pid, totals } };
    }
    std::uint64_t GetRetransmitBytes() const { std::lock_guard<std::mutex> lock(mutex); return retransmits; }
    std::vector<Traffic::NetworkEvent> Examples() const { std::lock_guard<std::mutex> lock(mutex); return examples; }
    void SetProbePort(std::uint16_t port) { std::lock_guard<std::mutex> lock(mutex); probePort = port; protocolPids.clear(); }
    void ReportTrace(std::ostream& out) const
    {
        std::vector<unsigned char> bytes(sizeof(EVENT_TRACE_PROPERTIES) + 4096);
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(bytes.data());
        properties->Wnode.BufferSize = static_cast<ULONG>(bytes.size());
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        const auto error = ControlTraceW(0, m_sessionName.c_str(), properties, EVENT_TRACE_CONTROL_QUERY);
        out << "trace_config\terror\t" << error << "\tflushSeconds\t" << properties->FlushTimer
            << "\tbufferKiB\t" << properties->BufferSize << "\tbuffers\t" << properties->NumberOfBuffers
            << "\tflags\t" << properties->EnableFlags << "\towned\t" << m_ownsSession << '\n';
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& row : protocolPids) out << "observed_port_pid\t" << row.first << '\t'
            << row.second.rxTotalBytes << '\t' << row.second.txTotalBytes << '\n';
    }
    void ReportRejected(std::ostream& out) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        out << "rejected-target\trx\t" << rejectedRx << "\ttx\t" << rejectedTx << "\tlastTimestamp\t" << rejectedTime
            << "\tcollectorStart\t" << m_startFileTime << '\n';
    }
};
struct SocketGuard
{
    SOCKET value{ INVALID_SOCKET };
    ~SocketGuard() { if (value != INVALID_SOCKET) closesocket(value); }
};
void SocketTimeout(SOCKET socket)
{
    DWORD timeout = 10000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}
void TransferSend(SOCKET socket, unsigned bytes, unsigned char fill)
{
    std::array<char, 65536> buffer;
    buffer.fill(static_cast<char>(fill));
    while (bytes)
    {
        const int sent = send(socket, buffer.data(), static_cast<int>((std::min)(bytes, static_cast<unsigned>(buffer.size()))), 0);
        Check(sent > 0, "Native TCP send failed or timed out");
        bytes -= sent;
    }
}
void TransferReceive(SOCKET socket, unsigned bytes, unsigned char fill)
{
    std::array<char, 65536> buffer;
    while (bytes)
    {
        const int received = recv(socket, buffer.data(), static_cast<int>((std::min)(bytes, static_cast<unsigned>(buffer.size()))), 0);
        Check(received > 0, "Native TCP receive failed or timed out");
        for (int i = 0; i < received; ++i) Check(static_cast<unsigned char>(buffer[i]) == fill, "Native TCP payload corrupted");
        bytes -= received;
    }
}
sockaddr_storage ProbeAddress(bool ipv6, unsigned short port)
{
    sockaddr_storage address{};
    if (ipv6)
    {
        auto& v6 = reinterpret_cast<sockaddr_in6&>(address);
        v6.sin6_family = AF_INET6;
        v6.sin6_addr = in6addr_loopback;
        v6.sin6_port = port;
    }
    else
    {
        auto& v4 = reinterpret_cast<sockaddr_in&>(address);
        v4.sin_family = AF_INET;
        v4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        v4.sin_port = port;
    }
    return address;
}
unsigned short AddressPort(const sockaddr_storage& address, bool ipv6)
{
    return ipv6 ? reinterpret_cast<const sockaddr_in6&>(address).sin6_port : reinterpret_cast<const sockaddr_in&>(address).sin_port;
}
int ProbeServer(bool ipv6, bool short_connections, bool steps = false)
{
    WSADATA wsa{};
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
    SocketGuard listener;
    listener.value = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Check(listener.value != INVALID_SOCKET, "Cannot create native TCP listener");
    auto address = ProbeAddress(ipv6, 0);
    int length = ipv6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    Check(bind(listener.value, reinterpret_cast<sockaddr*>(&address), length) != SOCKET_ERROR, "Cannot bind test loopback listener");
    Check(getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &length) == 0, "Cannot read listener port");
    Check(listen(listener.value, SOMAXCONN) == 0, "Cannot listen for test connection");
    std::cout << ntohs(AddressPort(address, ipv6)) << '\n' << std::flush;
    if (steps)
    {
        SocketGuard client;
        client.value = accept(listener.value, nullptr, nullptr);
        Check(client.value != INVALID_SOCKET, "Test accept failed");
        SocketTimeout(client.value);
        for (const auto size : probeSteps)
        {
            TransferReceive(client.value, size, 0x5a);
            TransferSend(client.value, size, 0x6b);
        }
        char eof;
        recv(client.value, &eof, 1, 0);
        return 0;
    }
    for (unsigned i = 0; i < (short_connections ? 100u : 1u); ++i)
    {
        SocketGuard client;
        client.value = accept(listener.value, nullptr, nullptr);
        Check(client.value != INVALID_SOCKET, "Test accept failed");
        SocketTimeout(client.value);
        TransferReceive(client.value, short_connections ? 1024 : probeUpload, 0x5a);
        TransferSend(client.value, short_connections ? 1024 : probeDownload, 0x6b);
        if (!short_connections)
        {
            char eof;
            recv(client.value, &eof, 1, 0); // Keep counters readable until the parent finishes its audit.
        }
    }
    return 0;
}
struct ProbeChild
{
    HANDLE process{}, pipe{};
    unsigned short port{};
    ProbeChild(bool ipv6, bool short_connections) : ProbeChild(ipv6, short_connections ? L"short" : L"long") {}
    ProbeChild(bool ipv6, const wchar_t* mode)
    {
        SECURITY_ATTRIBUTES attributes{ sizeof(attributes), nullptr, TRUE };
        HANDLE writer{};
        Check(CreatePipe(&pipe, &writer, &attributes, 0) != FALSE, "Cannot create diagnostic child pipe");
        SetHandleInformation(pipe, HANDLE_FLAG_INHERIT, 0);
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        auto command = L"\"" + std::wstring(executable) + L"\" --probe-server " +
            (ipv6 ? L"6 " : L"4 ") + mode;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writer; startup.hStdError = writer; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION info{};
        const auto started = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
        CloseHandle(writer);
        Check(started != FALSE, "Cannot launch isolated TCP peer");
        process = info.hProcess; CloseHandle(info.hThread);
        std::string line;
        for (unsigned i = 0; i < 32; ++i)
        {
            char c{}; DWORD read{};
            Check(ReadFile(pipe, &c, 1, &read, nullptr) && read, "Isolated TCP peer failed before listening");
            if (c == '\n') break;
            line.push_back(c);
        }
        port = htons(static_cast<unsigned short>(std::stoi(line)));
    }
    ~ProbeChild()
    {
        if (pipe) CloseHandle(pipe);
        if (process)
        {
            if (WaitForSingleObject(process, 12000) == WAIT_TIMEOUT) TerminateProcess(process, 98);
            CloseHandle(process);
        }
    }
};
void ConnectProbe(SocketGuard& client, bool ipv6, unsigned short port)
{
    client.value = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Check(client.value != INVALID_SOCKET, "Cannot create native TCP client");
    SocketTimeout(client.value);
    auto address = ProbeAddress(ipv6, port);
    Check(connect(client.value, reinterpret_cast<sockaddr*>(&address), ipv6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in)) == 0,
        "Cannot connect to isolated test peer");
}

void ShortProbe(std::ostream& out)
{
    NativeCollector etw;
    etw.m_includeLoopback = true;
    etw.Start(); Sleep(1200);
    const auto status = etw.GetStatusText();
    out << "short-status\t";
    for (auto c : status) out << static_cast<char>(c < 128 ? c : '?');
    out << "\trunning\t" << etw.m_running.load() << '\n' << std::flush;
    Check(etw.m_running.load(), "Short-connection ETW consumer did not start");
    ProbeChild child(false, true);
    for (unsigned i = 0; i < 100; ++i)
    {
        SocketGuard client;
        ConnectProbe(client, false, child.port);
        TransferSend(client.value, 1024, 0x5a);
        TransferReceive(client.value, 1024, 0x6b);
    }
    for (unsigned wait = 0; wait < 100; ++wait)
    {
        const auto total = etw.GetProcessTrafficSnapshot()[GetCurrentProcessId()];
        if (total.rxTotalBytes >= 102400 && total.txTotalBytes >= 102400) break;
        Sleep(100);
    }
    const auto total = etw.GetProcessTrafficSnapshot()[GetCurrentProcessId()];
    const auto diagnostics = etw.GetDiagnostics();
    out << "100-short\tknownRxTx\t102400\tETWRx\t" << total.rxTotalBytes << "\tETWTx\t" << total.txTotalBytes
        << "\taccepted\t" << diagnostics.accepted << "\tparseFailures\t" << diagnostics.parseFailures
        << "\tbufferedBeforeStart\t" << diagnostics.bufferedBeforeStart << "\teventsLost\t" << diagnostics.eventsLost << '\n' << std::flush;
    etw.ReportRejected(out);
    out << "clockType\t" << diagnostics.clockType << '\n' << std::flush;
    Check(total.rxTotalBytes == 102400 && total.txTotalBytes == 102400, "ETW short-connection coverage lost or duplicated payload");
    etw.Stop();
}
void NativeShortProbe(const wchar_t* output)
{
    std::ofstream out(output);
    WSADATA wsa{};
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
    try { ShortProbe(out); out << "PASS\n"; }
    catch (const std::exception& error) { out << "FAIL\t" << error.what() << '\n'; throw; }
    WSACleanup();
}

void NativeProbe(const wchar_t* output)
{
    std::ofstream out(output);
    Check(static_cast<bool>(out), "Cannot write native TCP report");
    WSADATA wsa{};
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
    try
    {
        for (const bool ipv6 : { false, true })
        {
            CHybridTrafficCollector hybrid;
            hybrid.m_etw.m_includeLoopback = true;
            hybrid.m_estats.m_includeLoopback = true;
            hybrid.m_includeProofLoopback = true;
            hybrid.m_etw.SetEventSink([](void* context, const Traffic::NetworkEvent& event) {
                if (event.key.pid == GetCurrentProcessId()) static_cast<CHybridTrafficCollector*>(context)->OnEvent(event);
            }, &hybrid);
            Check(hybrid.Start(), "Cannot start native handoff validation");
            NativeCollector etw;
            etw.m_includeLoopback = true;
            etw.Start();
            Sleep(1200);
            Check(etw.m_running.load() && etw.m_traceHandle.load() != INVALID_PROCESSTRACE_HANDLE, "ETW live consumer did not start");
            ProbeChild child(ipv6, false);
            SocketGuard client;
            ConnectProbe(client, ipv6, child.port);
            sockaddr_storage local{};
            int length = sizeof(local);
            getsockname(client.value, reinterpret_cast<sockaddr*>(&local), &length);
            const auto localPort = AddressPort(local, ipv6);
            CTcpEStatsCollector estats;
            estats.m_includeLoopback = true;
            bool found{};
            Traffic::ConnectionKey nativeKey;
            for (const auto& row : estats.Poll())
                if (row.key.pid == GetCurrentProcessId() && row.key.family == (ipv6 ? 6 : 4) && row.key.localPort == localPort)
                {
                    Check(row.enabled && !row.error && row.baseline, "Cannot establish a clean EStats baseline");
                    out << (ipv6 ? "IPv6" : "IPv4") << "\tbaselineRx\t" << row.rx << "\tbaselineTx\t" << row.tx << '\n' << std::flush;
                    found = true;
                    nativeKey = row.key;
                }
            Check(found, "Owner connection table omitted the native test connection");
            Sleep(1800);
            for (const auto& row : estats.Poll())
                if (row.key.pid == GetCurrentProcessId() && row.key.family == (ipv6 ? 6 : 4) && row.key.localPort == localPort)
                    out << "quiescent-baseline\t" << (ipv6 ? 6 : 4) << "\trx\t" << row.rx << "\ttx\t" << row.tx
                        << "\tidleRxDelta\t" << row.rxDelta << "\tidleTxDelta\t" << row.txDelta << '\n' << std::flush;
            const auto qualification = GetTickCount64() + 10000;
            while (!hybrid.GetDiagnostics().sources.active && GetTickCount64() < qualification) Sleep(100);
            Check(hybrid.GetDiagnostics().sources.active == 1, "Native bulk flow never obtained a verified zero-prefix handoff");
            NativePathProbe pathStats(nativeKey);
            const auto pathBefore = pathStats.Read();
            const auto before = etw.GetProcessTrafficSnapshot()[GetCurrentProcessId()];
            TransferSend(client.value, probeUpload, 0x5a);
            Sleep(300);
            for (const auto& connection : estats.m_connections)
                if (connection.first.pid == GetCurrentProcessId() && connection.first.localPort == localPort &&
                    connection.first.family == (ipv6 ? 6 : 4))
                {
                    const auto data = ReadProbeCounters(connection.first);
                    out << "after-upload-before-receive\t" << (ipv6 ? 6 : 4) << '\t' << data.DataBytesIn << '\t' << data.DataBytesOut
                        << '\t' << data.ThruBytesAcked << '\t' << data.ThruBytesReceived << '\n' << std::flush;
                }
            TransferReceive(client.value, probeDownload, 0x6b);
            Sleep(2500);
            Traffic::U64 rx{}, tx{};
            found = false;
            for (const auto& row : estats.Poll())
                if (!row.closed && row.key.pid == GetCurrentProcessId() && row.key.family == (ipv6 ? 6 : 4) && row.key.localPort == localPort)
                {
                    rx = row.rxDelta; tx = row.txDelta;
                    Check(!row.error && !row.baseline, "EStats test connection lost its baseline");
                    found = true;
                }
            Check(found, "EStats test connection disappeared before final sampling");
            const auto pathAfter = pathStats.Read();
            out << "native_path\t" << (ipv6 ? 6 : 4) << "\tBytesRetrans\t"
                << static_cast<DWORD>(pathAfter.BytesRetrans - pathBefore.BytesRetrans) << "\tPktsRetrans\t"
                << pathAfter.PktsRetrans - pathBefore.PktsRetrans << '\n';
            const auto callbackDeadline = GetTickCount64() + 7000;
            while (GetTickCount64() < callbackDeadline)
            {
                const auto delivered = etw.GetProcessTrafficSnapshot()[GetCurrentProcessId()];
                if (delivered.rxTotalBytes - before.rxTotalBytes >= probeDownload &&
                    delivered.txTotalBytes - before.txTotalBytes >= probeUpload) break;
                Sleep(100);
            }
            const auto after = etw.GetProcessTrafficSnapshot()[GetCurrentProcessId()];
            const auto d = etw.GetDiagnostics();
            for (const auto& connection : estats.m_connections)
            {
                if (connection.first.pid != GetCurrentProcessId() || connection.first.localPort != localPort ||
                    connection.first.family != (ipv6 ? 6 : 4)) continue;
                TCP_ESTATS_DATA_ROD_v0 details{};
                DWORD result;
                if (ipv6)
                {
                    auto row = Row6(connection.first);
                    result = GetPerTcp6ConnectionEStats(&row, TcpConnectionEstatsData, nullptr, 0, 0, nullptr, 0, 0,
                        reinterpret_cast<PUCHAR>(&details), 0, sizeof(details));
                }
                else
                {
                    auto row = Row4(connection.first);
                    result = GetPerTcpConnectionEStats(&row, TcpConnectionEstatsData, nullptr, 0, 0, nullptr, 0, 0,
                        reinterpret_cast<PUCHAR>(&details), 0, sizeof(details));
                }
                out << "raw\t" << result << "\tDataBytesIn\t" << details.DataBytesIn << "\tDataBytesOut\t" << details.DataBytesOut
                    << "\tThruBytesAcked\t" << details.ThruBytesAcked << "\tThruBytesReceived\t" << details.ThruBytesReceived
                    << "\tDataSegsIn\t" << details.DataSegsIn << "\tDataSegsOut\t" << details.DataSegsOut
                    << "\tSegsIn\t" << details.SegsIn << "\tSegsOut\t" << details.SegsOut
                    << "\tSndUna\t" << details.SndUna << "\tSndNxt\t" << details.SndNxt << "\tSndMax\t" << details.SndMax
                    << "\tRcvNxt\t" << details.RcvNxt << '\n';
            }
            for (const auto& example : etw.Examples())
                out << "event-sequence\t" << example.send << '\t' << example.sequence << '\t' << example.bytes << '\t' << example.connectionId << '\n';
            out << (ipv6 ? "IPv6" : "IPv4") << "\tknownRx\t" << probeDownload << "\tknownTx\t" << probeUpload
                << "\tETWRx\t" << after.rxTotalBytes - before.rxTotalBytes << "\tETWTx\t" << after.txTotalBytes - before.txTotalBytes
                << "\tEStatsRx\t" << rx << "\tEStatsTx\t" << tx << "\tEventsLost\t" << d.eventsLost
                << "\tETWRetransmitBytes\t" << etw.GetRetransmitBytes() << '\n' << std::flush;
            if (rx != probeDownload || tx != probeUpload)
            {
                out << "MISMATCH\traw Data counters are not verified application payload; a refusal is required\n" << std::flush;
            }
            Check(after.rxTotalBytes - before.rxTotalBytes == probeDownload && after.txTotalBytes - before.txTotalBytes == probeUpload,
                "ETW did not match known native TCP payload bytes");
            hybrid.Stop();
            Traffic::U64 formalRx{}, formalTx{};
            for (const auto& delta : hybrid.PeekPending().deltas) { formalRx += delta.rx; formalTx += delta.tx; }
            const auto hybridHealth = hybrid.GetDiagnostics();
            out << "hybrid\t" << (ipv6 ? 6 : 4) << "\trx\t" << formalRx << "\ttx\t" << formalTx
                << "\tswitches\t" << hybridHealth.sources.switches << "\trejected\t" << hybridHealth.sources.rejected
                << "\tfallbacks\t" << hybridHealth.sources.fallbacks << '\n' << std::flush;
            Check(formalRx == probeDownload && formalTx == probeUpload,
                "Verified bulk accounting did not preserve the known payload exactly");
            if (rx != probeDownload || tx != probeUpload)
                Check(hybridHealth.sources.rejected > 0, "A disagreeing native Data count was not rejected by the formal source");
            estats.Stop();
            etw.Stop();
        }
        ShortProbe(out);
        out << "PASS: known payload, actual verified handoff, raw counter disagreements rejected and nonduplicating fallback\n";
    }
    catch (const std::exception& error) { out << "FAIL\t" << error.what() << '\n'; throw; }
    WSACleanup();
}
}
