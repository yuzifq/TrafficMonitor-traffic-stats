#include <psapi.h>

namespace
{
std::uint64_t ProcessCpuTime()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    return (static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
        (static_cast<std::uint64_t>(user.dwHighDateTime) << 32) + user.dwLowDateTime;
}
PROCESS_MEMORY_COUNTERS_EX ProcessMemory()
{
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    return memory;
}
void NativeLoad(const wchar_t* path)
{
    std::ofstream out(path);
    Check(static_cast<bool>(out), "Cannot write load report");
    WSADATA wsa{};
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup failed");
    struct Sockets
    {
        std::vector<SOCKET> values;
        ~Sockets() { for (auto socket : values) closesocket(socket); }
    } sockets;
    sockets.values.reserve(512);
    SocketGuard listener;
    listener.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    auto address = ProbeAddress(false, 0);
    int length = sizeof(sockaddr_in);
    Check(bind(listener.value, reinterpret_cast<sockaddr*>(&address), length) == 0 &&
        listen(listener.value, SOMAXCONN) == 0 && getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &length) == 0,
        "Cannot prepare native load listener");
    const auto before = ProcessMemory();
    const auto cpu = ProcessCpuTime();
    const auto started = GetTickCount64();
    CHybridTrafficCollector collector;
    collector.m_estats.m_includeLoopback = true;
    collector.m_etw.m_includeLoopback = true;
    collector.m_includeProofLoopback = true;
    collector.Start();
    Sleep(1500);
    for (unsigned i = 0; i < 256; ++i)
    {
        SocketGuard client;
        ConnectProbe(client, false, AddressPort(address, false));
        SOCKET accepted = accept(listener.value, nullptr, nullptr);
        Check(accepted != INVALID_SOCKET, "Load peer accept failed");
        const int buffer = 4096;
        for (SOCKET endpoint : { client.value, accepted })
        {
            setsockopt(endpoint, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
            setsockopt(endpoint, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));
            sockets.values.push_back(endpoint);
        }
        client.value = INVALID_SOCKET;
    }
    for (unsigned i = 0; i < 12; ++i)
    {
        Sleep(1000);
        const auto d = collector.GetDiagnostics();
        const auto memory = ProcessMemory();
        out << i << '\t' << d.estats.connections << '\t' << d.estats.enabled << '\t' << d.estats.polls << '\t' << d.estats.pollMicros
            << '\t' << memory.WorkingSetSize << '\t' << memory.PrivateUsage << '\t' << d.queueOverflows
            << '\t' << d.sources.active << '\t' << d.proof.parseFailures << '\n' << std::flush;
    }
    const auto elapsed = GetTickCount64() - started;
    const auto consumed = ProcessCpuTime() - cpu;
    const auto after = ProcessMemory();
    const auto diagnostics = collector.GetDiagnostics();
    out << "summary\tconnections\t" << diagnostics.estats.connections << "\tseconds\t" << elapsed / 1000.0
        << "\tcpuSeconds\t" << consumed / 10000000.0 << "\tworkingSetDelta\t" << static_cast<long long>(after.WorkingSetSize) - before.WorkingSetSize
        << "\tprivateDelta\t" << static_cast<long long>(after.PrivateUsage) - before.PrivateUsage << '\n';
    collector.Stop();
    Check(diagnostics.estats.enabled >= 512, "Load test did not enable its 512 controlled TCP endpoint rows");
    Check(diagnostics.estats.polls >= 16 && !diagnostics.queueOverflows, "Load test stalled polling or exceeded the connection queue");
    Check(diagnostics.sources.active >= 200 && !diagnostics.proof.parseFailures && !diagnostics.proof.eventsLost,
        "Load test never exercised real source selection or lost proof events");
    out << "PASS\n";
}
}
