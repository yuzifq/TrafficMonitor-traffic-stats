#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "../ProcessTrafficPlugin/NetworkTrafficStore.h"
#include "../ProcessTrafficPlugin/PluginInterface.h"
#include "../ProcessTrafficPlugin/Utils.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace
{
using Collector = CNetworkTrafficCollector;
using Store = CNetworkTrafficStore;
using U64 = std::uint64_t;

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

U64 Time(WORD year, WORD month, WORD day, WORD hour = 12, WORD minute = 0)
{
    SYSTEMTIME time{};
    time.wYear = year; time.wMonth = month; time.wDay = day; time.wHour = hour; time.wMinute = minute;
    FILETIME file{};
    Check(SystemTimeToFileTime(&time, &file) != FALSE, "Invalid test time");
    return (static_cast<U64>(file.dwHighDateTime) << 32 | file.dwLowDateTime) / 10000;
}

Collector::Adapter Adapter(const wchar_t* id, U64 rx, U64 tx, bool connected = true, bool hardware = true)
{
    return { id, id, 1, rx, tx, connected, hardware };
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

void Write(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    Check(output.good(), "Could not write fixture");
}

std::filesystem::path DailyFile(const std::filesystem::path& fixture)
{
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixture / L"network_traffic_history"))
        if (entry.path().extension() == L".tsv" && entry.path().stem().wstring().size() == 10) return entry.path();
    throw std::runtime_error("No daily network file");
}

void CheckCollector()
{
    Collector collector;
    const auto base = Time(2024, 1, 1);
    auto a = Adapter(L"A", 1ULL << 40, 1ULL << 41);
    auto virtual_adapter = Adapter(L"Virtual", 1ULL << 50, 1ULL << 50, true, false);
    Check(!collector.Observe({ a, virtual_adapter }, 1000, base), "First sample counted boot traffic");
    Check(collector.GetSnapshot().activeName == L"A", "Automatic selection preferred a tunnel over hardware");
    a.rxBytes += 8192; a.txBytes += 1024;
    const auto sample = collector.Observe({ a, virtual_adapter }, 3000, base + 2000);
    Check(sample && sample->rxBytes == 8192 && sample->txBytes == 1024 && collector.GetSnapshot().rxPerSec == 4096,
        "64-bit delta or elapsed-time rate is wrong");
    for (int i = 0; i < 100; ++i) collector.GetSnapshot();
    a.rxBytes += 123; a.txBytes += 456;
    Check(!collector.Observe({ a }, 3000, base + 2000), "Duplicate timestamp produced a second sample");
    const auto next = collector.Observe({ a }, 4000, base + 3000);
    Check(next && next->rxBytes == 123 && next->txBytes == 456, "Display reads consumed traffic");
    auto b = Adapter(L"B", 1ULL << 55, 1ULL << 55);
    a.rxBytes += 10;
    Check(collector.Observe({ a, b }, 5000, base + 4000)->adapterId == L"A", "Automatic selection flapped");
    a.rxBytes = 4;
    Check(!collector.Observe({ a }, 6000, base + 5000), "Counter reset underflowed");
    a.rxBytes += 10;
    Check(collector.Observe({ a }, 7000, base + 6000)->rxBytes == 10, "Reset baseline did not recover");
    collector.SelectAdapter(L"B");
    Check(!collector.Observe({ a, b }, 8000, base + 7000), "Adapter change counted another adapter's counters");
    b.instance = 2;
    b.rxBytes += 10;
    Check(!collector.Observe({ b }, 9000, base + 8000), "Reused interface instance did not reset baseline");
    b.connected = false;
    Check(!collector.Observe({ b }, 10000, base + 9000), "Disconnected adapter produced traffic");
    b.connected = true;
    Check(!collector.Observe({ b }, 11000, base + 10000), "Reconnect did not establish a baseline");
    Check(!collector.Observe({}, 12000, base + 11000), "Missing adapter produced traffic");
    Check(!collector.Observe({ b }, 13000, base + 12000), "Missing sample gap was bridged");
    b.rxBytes += 42;
    Check(!collector.Observe({ b }, 14000, base + 3600000), "Wall-clock jump produced a false interval");
    b.rxBytes += 8;
    const auto after_jump = collector.Observe({ b }, 15000, base + 3601000);
    Check(after_jump && after_jump->rxBytes == 8, "Clock jump did not recover");
    b.rxBytes += 12345;
    const auto coarse = collector.Observe({ b }, 35000, base + 3621000);
    Check(coarse && coarse->coarse && coarse->rxBytes == 12345, "Long interval was lost or presented as minute precision");
    Check(!collector.Observe({ b }, 36000, base), "Backward clock jump was accepted");
    std::cout << "PASS: independent 64-bit sampling, stable adapter selection, rates, resets, reconnect, missing reads, clock jumps, coarse intervals\n";
}

void CheckStore(const std::filesystem::path& fixture)
{
    const auto base = Time(2024, 2, 29);
    Store store;
    store.Initialize(fixture.wstring());
    Check(store.SetPreferredAdapter(L"A"), "Cannot save adapter preference");
    Check(store.Add({ base, base + 1000, 100, 10, L"A", false }), "Cannot add first sample");
    Check(store.Add({ base + 1000, base + 59000, 200, 20, L"A", false }), "Cannot merge a minute");
    Check(store.Add({ base + 59000, base + 61000, 50, 5, L"A", false }), "Cannot keep boundary interval");
    Check(store.Add({ base + 61000, base + 62000, 30, 3, L"A", false }), "Cannot add next minute");
    const auto minute = store.GetRangeUtc(base + 60000, base + 120000);
    Check(minute.amount.rxBytes == 30 && minute.boundary.rxBytes == 50 && minute.coveredMs == 2000 &&
        minute.requestedMs == 60000 && minute.available, "Minute boundary bytes were guessed or gaps hidden");
    const auto full = store.GetRangeUtc(base, base + 120000);
    Check(full.amount.rxBytes == 380 && full.amount.txBytes == 38 && full.coveredMs == 62000,
        "Interval totals or coverage changed");
    Check(!store.GetRangeUtc(base - 60000, base).available, "Missing history was reported as measured zero");
    Check(store.Flush(), "Cannot checkpoint current record");
    const auto pending_path = fixture / L"network_traffic_history" / L"pending.tsv";
    const auto checkpoint = Read(pending_path);
    Check(store.Flush(true), "Cannot commit records");
    Write(pending_path, checkpoint);
    Store recovered;
    recovered.Initialize(fixture.wstring());
    Check(recovered.GetAllTimeTotal().rxBytes == 380 && recovered.GetPreferredAdapter() == L"A",
        "Restart replay duplicated a committed record or lost preference");
    Check(recovered.Flush(true) && recovered.GetAllTimeTotal().rxBytes == 380, "Repeated flush duplicated history");
    const auto day = DailyFile(fixture);
    auto summary = day; summary += L".sum";
    Write(summary, "damaged summary");
    Store fallback;
    fallback.Initialize(fixture.wstring());
    Check(fallback.GetAllTimeTotal().rxBytes == 380, "Corrupt summary did not fall back to raw records");
    {
        std::ofstream append(day, std::ios::binary | std::ios::app);
        append << "incomplete row";
    }
    Store partial;
    partial.Initialize(fixture.wstring());
    Check(!partial.GetRangeUtc(base, base + 120000).healthy, "Incomplete tail was hidden");
    Check(partial.Add({ base + 120000, base + 121000, 20, 2, L"A", false }) && partial.Flush(true),
        "Could not recover a truncated append tail");
    Check(partial.GetAllTimeTotal().rxBytes == 400 && partial.GetRangeUtc(base, base + 180000).healthy,
        "Tail recovery removed valid history");
    HANDLE locked = CreateFileW(day.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "Cannot lock test history");
    partial.Add({ base + 180000, base + 181000, 40, 4, L"A", false });
    Check(!partial.Flush(true), "Locked append unexpectedly succeeded");
    Check(partial.GetAllTimeTotal().rxBytes == 440, "Failed write lost or duplicated in-memory bytes");
    CloseHandle(locked);
    Check(partial.Flush(true) && partial.Flush(true) && partial.GetAllTimeTotal().rxBytes == 440,
        "Write retry duplicated or dropped bytes");
    Check(partial.Add({ base + 181000, base + 182000, 0, 0, L"B", false }), "Cannot record an idle interval");
    Check(partial.GetRangeUtc(base + 181000, base + 182000).available, "Measured idle traffic was treated as missing");
    const auto local_range = [&] {
        FILETIME file{};
        ULARGE_INTEGER value{}; value.QuadPart = base * 10000;
        file.dwLowDateTime = value.LowPart; file.dwHighDateTime = value.HighPart;
        SYSTEMTIME utc{}, local{};
        FileTimeToSystemTime(&file, &utc);
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
        Store::Range range{ local, local }; range.end.wMinute += 4; return range;
    }();
    std::wstring error;
    Check(partial.ExportRange(local_range, (fixture / L"network-export.tsv").wstring(), error), "Network export failed");
    const auto exported = Read(fixture / L"network-export.tsv");
    Check(exported.find("StartUtc\tEndUtc") == 0 && exported.find("\tA\tsampled\tinside") != std::string::npos,
        "Export lost actual time intervals or adapter identity");
    Check(!partial.ExportRange(local_range, day.wstring(), error), "Export overwrote managed network history");
    Check(partial.IsManagedPath((fixture / L"app_traffic_history" / L"apps.tsv").wstring()), "Process history is not protected");
    Store relocated;
    relocated.Initialize((fixture / L"history").wstring());
    Check(relocated.IsManagedPath((fixture / L"history_traffic.dat").wstring()) &&
        relocated.IsManagedPath((fixture / L"history_traffic.dat.bak").wstring()) &&
        relocated.IsManagedPath((fixture / L"app_traffic_state.tsv").wstring()) &&
        relocated.IsManagedPath((fixture / L"history/app_traffic_state.tsv.tmp").wstring()) &&
        !relocated.IsManagedPath((fixture / L"export.tsv").wstring()),
        "Relocated storage lost managed/official export protection or blocked ordinary exports");
    const auto corrupt_fixture = fixture / L"corrupt-checkpoint";
    Store corrupt;
    corrupt.Initialize(corrupt_fixture.wstring());
    const auto corrupt_path = corrupt_fixture / L"network_traffic_history" / L"pending.tsv";
    Write(corrupt_path, "damaged checkpoint\n");
    Store damaged;
    damaged.Initialize(corrupt_fixture.wstring());
    damaged.Add({ base, base + 1000, 10, 1, L"A", false });
    Check(!damaged.Flush(true) && !damaged.Healthy() && Read(corrupt_path) == "damaged checkpoint\n",
        "Damaged recovery data was silently overwritten");
    std::cout << "PASS: minute boundaries, missing/idle coverage, restart replay, summary fallback, partial-tail repair, locked writes/retry, preference, streaming export\n";
}

void CheckMidnight(const std::filesystem::path& fixture)
{
    const auto base = Time(2024, 12, 31, 15, 59);
    Store store;
    store.Initialize(fixture.wstring());
    Check(store.Add({ base, base + 59000, 100, 10, L"A", false }), "Midnight add failed");
    Check(store.Add({ base + 59000, base + 61000, 200, 20, L"A", false }), "Cross-day sample failed");
    Check(store.Add({ base + 61000, base + 62000, 300, 30, L"A", false }) && store.Flush(true), "Next-day sample failed");
    Store reopened;
    reopened.Initialize(fixture.wstring());
    Check(reopened.GetRangeUtc(base, base + 120000).amount.rxBytes == 600,
        "Year/day rollover dropped a cross-day interval");
    std::cout << "PASS: leap-day storage and year/day rollover\n";
}

void CheckInterruptedCreation(const std::filesystem::path& fixture)
{
    const auto base = Time(2024, 5, 1);
    const auto header_fixture = fixture / L"header";
    Store initial;
    initial.Initialize(header_fixture.wstring());
    initial.Add({ base, base + 1000, 10, 1, L"A", false });
    Check(initial.Flush(true), "Cannot create header fixture");
    const auto path = DailyFile(header_fixture);
    for (const auto& prefix : { std::string{}, std::string("netw"), std::string("network\t1") })
    {
        Write(path, prefix);
        Store resumed;
        resumed.Initialize(header_fixture.wstring());
        resumed.Add({ base + 60000, base + 61000, 30, 3, L"A", false });
        Check(resumed.Flush(true) && resumed.GetAllTimeTotal().rxBytes == 30 &&
            Read(path).find("network\t1\n") == 0, "Interrupted first-file header could not be repaired");
    }
    const auto committed_fixture = fixture / L"committed";
    Store committed;
    committed.Initialize(committed_fixture.wstring());
    committed.Add({ base, base + 1000, 10, 1, L"A", false });
    Check(committed.Flush(true), "Cannot create committed fixture");
    const auto committed_path = DailyFile(committed_fixture);
    const auto committed_data = Read(committed_path);
    const auto other_fixture = fixture / L"other";
    Store other;
    other.Initialize(other_fixture.wstring());
    other.Add({ base + 60000, base + 61000, 40, 4, L"A", false });
    Check(other.Flush(), "Cannot create conflicting checkpoint");
    const auto checkpoint = Read(other_fixture / L"network_traffic_history" / L"pending.tsv");
    const auto checkpoint_path = committed_fixture / L"network_traffic_history" / L"pending.tsv";
    Write(checkpoint_path, checkpoint);
    Store collision;
    collision.Initialize(committed_fixture.wstring());
    Check(!collision.Healthy() && !collision.Flush(true) && Read(committed_path) == committed_data && Read(checkpoint_path) == checkpoint,
        "Conflicting commit ID was silently discarded or counted");
    std::cout << "PASS: interrupted file creation, durable replay checks, conflicting commit IDs preserve original data\n";
}

void LiveCheck()
{
    Collector collector;
    collector.Sample();
    auto first = collector.GetSnapshot();
    Check(!first.activeName.empty(), "No active adapter for live check");
    const auto chosen = [&] {
        for (const auto& adapter : first.adapters) if (adapter.name == first.activeName) return adapter;
        throw std::runtime_error("Missing live adapter");
    }();
    collector.SelectAdapter(chosen.id);
    NET_LUID luid{};
    luid.Value = chosen.instance;
    NET_IFINDEX index{};
    Check(ConvertInterfaceLuidToIndex(&luid, &index) == NO_ERROR, "Cannot find legacy interface index");
    const auto legacy = [&] {
        MIB_IFROW row{};
        row.dwIndex = index;
        Check(GetIfEntry(&row) == NO_ERROR, "Cannot read legacy adapter counters");
        return row;
    };
    const auto legacy_start_before = legacy();
    collector.Sample();
    const auto legacy_start_after = legacy();
    const auto baseline = collector.GetSnapshot();
    const auto before = [&] {
        for (const auto& adapter : baseline.adapters) if (adapter.id == chosen.id) return adapter;
        throw std::runtime_error("Missing baseline");
    }();
    U64 rx{}, tx{};
    MIB_IFROW legacy_end_before{}, legacy_end_after{};
    for (int i = 0; i < 40; ++i)
    {
        Sleep(250);
        if (i == 39) legacy_end_before = legacy();
        const auto interval = collector.Sample();
        if (i == 39) legacy_end_after = legacy();
        Check(interval.has_value(), "Live sampling lost its baseline");
        rx += interval->rxBytes; tx += interval->txBytes;
    }
    const auto last = collector.GetSnapshot();
    for (const auto& adapter : last.adapters)
    {
        if (adapter.id != chosen.id) continue;
        Check(rx == adapter.rxBytes - before.rxBytes && tx == adapter.txBytes - before.txBytes,
            "Live totals diverged from Windows counters");
        const auto rx_low = static_cast<DWORD>(legacy_end_before.dwInOctets - legacy_start_after.dwInOctets);
        const auto rx_high = static_cast<DWORD>(legacy_end_after.dwInOctets - legacy_start_before.dwInOctets);
        const auto tx_low = static_cast<DWORD>(legacy_end_before.dwOutOctets - legacy_start_after.dwOutOctets);
        const auto tx_high = static_cast<DWORD>(legacy_end_after.dwOutOctets - legacy_start_before.dwOutOctets);
        Check(rx >= rx_low && rx <= rx_high && tx >= tx_low && tx <= tx_high,
            "64-bit collection disagrees with the legacy counters used by the host");
        std::wcout << L"PASS: live adapter=" << chosen.name << L" rx=" << rx << L" tx=" << tx << L" bytes; 40 samples equal Windows counter deltas\n";
        std::wcout << L"PASS: legacy counter brackets rx=" << rx_low << L".." << rx_high << L" tx=" << tx_low << L".." << tx_high << L" bytes\n";
    }
}

void CheckDll(const wchar_t* dll_path, bool collect = true)
{
    const auto fixture = std::filesystem::absolute(std::filesystem::path(L"build") /
        (L"dll-checks-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(fixture);
    const auto official = fixture / L"history_traffic.dat";
    Write(official, "host history sentinel\n");
    const auto storage = std::filesystem::path(Utils::GetHistoryDirectory());
    const auto host_directory = storage.parent_path();
    const auto root_state = Read(host_directory / L"app_traffic_state.tsv");
    class Host final : public ITrafficMonitor
    {
    public:
        unsigned reads{};
        std::wstring directory;
        int GetAPIVersion() override { return 0; }
        const wchar_t* GetVersion() override { return L"1.86"; }
        double GetMonitorValue(MonitorItem) override { ++reads; return 1e30; }
        const wchar_t* GetMonitorValueString(MonitorItem, int) override { ++reads; return L"invalid host value"; }
        void ShowNotifyMessage(const wchar_t*) override {}
        WORD GetLanguageId() const override { return 0x409; }
        const wchar_t* GetPluginConfigDir() const override { return directory.c_str(); }
        int GetDPI(DPIType) const override { return 96; }
        COLORREF GetThemeColor() const override { return 0; }
        const wchar_t* GetStringRes(const wchar_t*, const wchar_t*) override { return L""; }
    } host;
    host.directory = fixture.wstring();
    HMODULE library = LoadLibraryW(std::filesystem::absolute(dll_path).c_str());
    if (!library) throw std::runtime_error("Release DLL failed to load; Win32 error " + std::to_string(GetLastError()));
    using Entry = ITMPlugin* (*)();
    const auto entry = reinterpret_cast<Entry>(GetProcAddress(library, "TMPluginGetInstance"));
    Check(entry != nullptr, "Plugin entry export is missing");
    auto* plugin = entry();
    Check(plugin && plugin->GetAPIVersion() == 7, "Plugin API changed");
    // Hosts may ask for names/language before supplying their config directory.
    Check(plugin->GetInfo(ITMPlugin::TMI_NAME) != nullptr, "Early plugin metadata failed");
    plugin->OnInitialize(&host);
    plugin->OnExtenedInfo(ITMPlugin::EI_CONFIG_DIR, host.directory.c_str());
    Check(std::filesystem::exists(storage / L"app_traffic_state.tsv") &&
        std::filesystem::exists(storage / L"network_traffic_history") &&
        !std::filesystem::exists(fixture / L"app_traffic_state.tsv") &&
        !std::filesystem::exists(fixture / L"network_traffic_history") &&
        Read(host_directory / L"app_traffic_state.tsv") == root_state,
        "Early metadata or late config selected storage outside host/history");
    Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_VERSION)) == L"1.2.0", "Plugin version is not 1.2.0");
    Check(plugin->GetItem(0) && plugin->GetItem(9) && !plugin->GetItem(10), "Process ranking items changed");
    if (!collect)
    {
        std::cout << "PASS: actual Release DLL load, API 7, version, ranking, early metadata and late config use host/history without starting collectors\n";
        return;
    }
    plugin->DataRequired();
    Sleep(1000);
    plugin->DataRequired();
    Check(host.reads == 0 && Read(official) == "host history sentinel\n", "Plugin reused official statistics");
    const auto checkpoint = Read(storage / L"network_traffic_history" / L"pending.tsv");
    Check(checkpoint.find("network\t1\n") == 0 && checkpoint.size() > 10, "Release DLL did not persist an independent sample");
    // The host owns plugin lifetime. Keep the module loaded until this smoke-test process exits.
    std::cout << "PASS: actual Release DLL load, API 7, version, ranking items, independent host callbacks and persisted network sample\n";
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring(argv[1]) == L"--live") { LiveCheck(); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--dll") { CheckDll(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--dll-load") { CheckDll(argv[2], false); return 0; }
        const auto fixture = std::filesystem::absolute(std::filesystem::path(L"build") /
            (L"network-checks-" + std::to_wstring(GetTickCount64())) / L"\u4e2d\u6587");
        CheckCollector();
        CheckStore(fixture);
        CheckMidnight(fixture / L"rollover");
        CheckInterruptedCreation(fixture / L"interrupted");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
