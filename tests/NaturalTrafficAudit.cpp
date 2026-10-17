#include <netioapi.h>
#include <objbase.h>

namespace
{
struct AuditRaw
{
    struct Count { Traffic::U64 rx{}, tx{}, receiveEvents{}, sendEvents{}, retransmit{}; };
    std::mutex mutex;
    std::map<std::pair<unsigned, DWORD>, Count> rows;
    std::map<Traffic::U64, Count> seconds;
    std::map<Traffic::ConnectionKey, Count> flows;
    std::atomic<Traffic::U64> begin{}, end{ (std::numeric_limits<Traffic::U64>::max)() };

    static void Event(void* context, const Traffic::NetworkEvent& event)
    {
        auto& audit = *static_cast<AuditRaw*>(context);
        if (event.preStart || event.lifecycle || event.timestamp < audit.begin || event.timestamp >= audit.end) return;
        std::lock_guard<std::mutex> lock(audit.mutex);
        auto& row = audit.rows[{ event.key.protocol, event.key.pid }];
        if (event.retransmit) { row.retransmit += event.bytes; return; }
        (event.send ? row.tx : row.rx) += event.bytes;
        ++(event.send ? row.sendEvents : row.receiveEvents);
        auto& second = audit.seconds[event.timestamp / 10000000];
        (event.send ? second.tx : second.rx) += event.bytes;
        auto& flow = audit.flows[event.key];
        (event.send ? flow.tx : flow.rx) += event.bytes;
    }

    void Write(std::ostream& out, const char* label)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& row : rows)
            out << label << '\t' << row.first.first << '\t' << row.first.second << '\t'
                << row.second.rx << '\t' << row.second.tx << '\t' << row.second.receiveEvents << '\t'
                << row.second.sendEvents << '\t' << row.second.retransmit << '\n';
    }
};

void AuditAdapters(std::ostream& out)
{
    MIB_IF_TABLE2* table{};
    const auto error = GetIfTable2(&table);
    if (error) { out << "adapter_error\t" << error << '\n'; return; }
    const auto at = Traffic::FileTimeNow();
    for (ULONG i = 0; i < table->NumEntries; ++i)
    {
        const auto& row = table->Table[i];
        wchar_t guid[40]{};
        StringFromGUID2(row.InterfaceGuid, guid, 40);
        out << "adapter\t" << at << '\t' << row.InterfaceIndex << '\t';
        for (const auto c : std::wstring(guid)) out << static_cast<char>(c);
        out << '\t' << row.InOctets << '\t' << row.OutOctets << '\t'
            << row.InUcastPkts << '\t' << row.InNUcastPkts << '\t'
            << row.OutUcastPkts << '\t' << row.OutNUcastPkts << '\t'
            << row.InDiscards << '\t' << row.OutDiscards << '\t'
            << row.OperStatus << '\t' << static_cast<unsigned>(row.InterfaceAndOperStatusFlags.HardwareInterface) << '\n';
    }
    FreeMibTable(table);
}

void NaturalTrafficAudit(const wchar_t* path, bool capture = false)
{
    std::ofstream out(path);
    Check(static_cast<bool>(out), "Cannot open natural traffic audit report");
    CHybridTrafficCollector hybrid;
    Check(hybrid.Start(), "Cannot start diagnostic hybrid collector");
    AuditRaw raw;
    CEtwProcNetCollector legacy;
    legacy.SetEventSink(AuditRaw::Event, &raw);
    Check(legacy.Start(), "Cannot attach read-only raw ETW consumer");
    if (capture)
    {
        const auto ready = std::filesystem::path(path).wstring() + L".capture-ready";
        const auto deadline = GetTickCount64() + 20000;
        while (!std::filesystem::exists(ready))
        {
            Check(GetTickCount64() < deadline, "Packet capture did not become ready");
            Sleep(50);
        }
    }
    raw.begin = Traffic::FileTimeNow();
    out << "begin\t" << raw.begin.load() << '\n';
    AuditAdapters(out);
    Traffic::U64 rx{}, tx{};
    for (unsigned i = 0; i < 90; ++i)
    {
        Sleep(1000);
        const auto batch = hybrid.PeekPending();
        for (const auto& delta : batch.deltas) { rx += delta.rx; tx += delta.tx; }
        hybrid.Acknowledge(batch.sequence);
        const auto d = hybrid.GetDiagnostics();
        out << "hybrid\t" << Traffic::FileTimeNow() << '\t' << rx << '\t' << tx << '\t'
            << d.sources.switches << '\t' << d.sources.active << '\t' << d.sources.fallbacks << '\t'
            << d.sources.rejected << '\t' << d.sources.overflows << '\t' << d.sources.gaps << '\t'
            << d.sources.estatsRx << '\t' << d.sources.estatsTx << '\t'
            << d.sources.suppressedRx << '\t' << d.sources.suppressedTx << '\t'
            << d.proof.error << '\t' << d.proof.parseFailures << '\t' << d.proof.eventsLost << '\t'
            << d.proof.buffersLost << '\t' << d.etw.eventsLost << '\t' << d.etw.parseFailures << '\t'
            << d.etw.endpointFailures << '\t' << d.etw.realtimeBuffersLost << '\t'
            << d.estats.enableFailures << '\t' << d.estats.readFailures << '\t'
            << d.estats.lastError << '\t' << d.commitFailures << '\n';
        AuditAdapters(out);
        raw.Write(out, "raw_progress");
        out.flush();
    }
    raw.end = Traffic::FileTimeNow();
    out << "end\t" << raw.end.load() << '\n';
    out.flush();
    if (capture) std::ofstream(std::filesystem::path(path).wstring() + L".quit") << "done\n";
    // Allow callbacks buffered by the existing kernel session to arrive, but
    // never count traffic generated after the fixed audit window.
    Sleep(2500);
    legacy.Stop();
    raw.Write(out, "raw_final");
    const auto d = legacy.GetDiagnostics();
    out << "raw_health\t" << d.eventsLost << '\t' << d.realtimeBuffersLost << '\t'
        << d.parseFailures << '\t' << d.endpointFailures << '\t' << d.retransmitBytes << '\n';
    hybrid.Stop();
    out << "done\n";
}

void OfficialCountersAudit(const wchar_t* path)
{
    std::ofstream out(path);
    Check(static_cast<bool>(out), "Cannot open adapter counter audit report");
    auto root = std::filesystem::absolute(path).parent_path();
    while (!std::filesystem::exists(root / L"TrafficMonitor.exe"))
    {
        Check(root.parent_path() != root, "TrafficMonitor.exe not found above report directory");
        root = root.parent_path();
    }
    const auto history = root / L"history_traffic.dat";
    for (unsigned i = 0; i < 65; ++i)
    {
        AuditAdapters(out);
        DWORD size{};
        Check(GetIfTable(nullptr, &size, FALSE) == ERROR_INSUFFICIENT_BUFFER, "Legacy adapter table size unavailable");
        std::vector<unsigned char> storage(size);
        auto* table = reinterpret_cast<MIB_IFTABLE*>(storage.data());
        Check(GetIfTable(table, &size, FALSE) == NO_ERROR, "Legacy adapter table unavailable");
        const auto at = Traffic::FileTimeNow();
        for (DWORD j = 0; j < table->dwNumEntries; ++j)
            out << "legacy_adapter\t" << at << '\t' << table->table[j].dwIndex << '\t'
                << table->table[j].dwInOctets << '\t' << table->table[j].dwOutOctets << '\n';
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        Check(GetFileAttributesExW(history.c_str(), GetFileExInfoStandard, &attributes) != FALSE,
            "Official history file unavailable");
        const auto written = static_cast<Traffic::U64>(attributes.ftLastWriteTime.dwHighDateTime) << 32 |
            attributes.ftLastWriteTime.dwLowDateTime;
        std::ifstream file(history);
        std::string line;
        std::getline(file, line); std::getline(file, line);
        out << "official\t" << Traffic::FileTimeNow() << '\t' << written << '\t' << line << '\n';
        out.flush();
        Sleep(1000);
    }
    out << "done\n";
}

void RawTrafficAudit(const wchar_t* path, bool capture = false)
{
    std::ofstream out(path);
    Check(static_cast<bool>(out), "Cannot open raw ETW audit report");
    AuditRaw raw;
    raw.begin = (std::numeric_limits<Traffic::U64>::max)();
    CEtwProcNetCollector legacy;
    legacy.SetEventSink(AuditRaw::Event, &raw);
    Check(legacy.Start(), "Cannot attach read-only ETW audit consumer");
    if (capture)
    {
        const auto ready = std::filesystem::path(path).wstring() + L".capture-ready";
        const auto deadline = GetTickCount64() + 20000;
        while (!std::filesystem::exists(ready))
        {
            Check(GetTickCount64() < deadline, "Packet capture did not become ready");
            Sleep(50);
        }
    }
    AuditAdapters(out);
    raw.begin = Traffic::FileTimeNow();
    out << "begin\t" << raw.begin.load() << '\n';
    for (unsigned i = 0; i < 180; ++i)
    {
        Sleep(1000);
        AuditAdapters(out);
        out.flush();
    }
    raw.end = Traffic::FileTimeNow();
    out << "end\t" << raw.end.load() << '\n';
    out.flush();
    if (capture) std::ofstream(std::filesystem::path(path).wstring() + L".quit") << "done\n";
    Sleep(2500);
    legacy.Stop();
    raw.Write(out, "raw_final");
    for (const auto& row : raw.seconds)
        out << "second\t" << row.first << '\t' << row.second.rx << '\t' << row.second.tx << '\n';
    for (const auto& row : raw.flows)
    {
        const auto address = [&](const auto& bytes) {
            constexpr char hex[] = "0123456789abcdef";
            for (const auto byte : bytes) out << hex[byte >> 4] << hex[byte & 15];
        };
        out << "flow\t" << static_cast<unsigned>(row.first.protocol) << '\t'
            << static_cast<unsigned>(row.first.family) << '\t' << row.first.pid << '\t';
        address(row.first.local); out << '\t' << ntohs(row.first.localPort) << '\t';
        address(row.first.remote); out << '\t' << ntohs(row.first.remotePort) << '\t'
            << row.second.rx << '\t' << row.second.tx << '\n';
    }
    const auto d = legacy.GetDiagnostics();
    out << "raw_health\t" << d.eventsLost << '\t' << d.realtimeBuffersLost << '\t'
        << d.parseFailures << '\t' << d.endpointFailures << '\n';
    out << "done\n";
}
}
