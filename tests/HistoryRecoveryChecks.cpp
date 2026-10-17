#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

SYSTEMTIME fakeNow{ 2026, 10, 5, 16, 10, 0, 0, 0 };
void WINAPI FixtureLocalTime(SYSTEMTIME* value) { *value = fakeNow; }
#define private public
#include "../ProcessTrafficPlugin/HistoryTrafficStore.h"
#undef private
#define GetLocalTime FixtureLocalTime
#include "../ProcessTrafficPlugin/HistoryTrafficStore.cpp"
#undef GetLocalTime

namespace
{
void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
std::string Contents(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
CHistoryTrafficStore::DateTimeRange Day()
{
    auto first = fakeNow, last = fakeNow;
    first.wHour = first.wMinute = 0;
    last.wHour = 23; last.wMinute = 59;
    return { first, last };
}
CHistoryTrafficStore::AppTotalEntry App(std::uint64_t rx, std::uint64_t tx, const std::wstring& path = L"C:\\One\\same.exe")
{
    CHistoryTrafficStore::AppTotalEntry app{ L"same.exe", path, rx, tx };
    app.identity = L"path:" + path;
    FILETIME timestamp{};
    SystemTimeToFileTime(&fakeNow, &timestamp);
    app.sampleStart = (static_cast<std::uint64_t>(timestamp.dwHighDateTime) << 32) | timestamp.dwLowDateTime;
    app.sampleEnd = app.sampleStart + 10000;
    return app;
}
void IdentityAndLegacy(const std::filesystem::path& fixture)
{
    fakeNow.wHour = 10; fakeNow.wMinute = 0;
    const auto root = fixture / L"app_traffic_history";
    const auto raw = root / L"2026/10/2026-10-16.tsv";
    std::filesystem::create_directories(raw.parent_path());
    std::ofstream(root / L"apps.tsv") << "version\t4\napp\t1\tlegacy.exe\n";
    const std::string original = "09:00\t1\ta\tb\n";
    std::ofstream(raw, std::ios::binary) << original;
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    Check(store.GetRangeTotal(Day()).rxBytes == 10, "Legacy fixture was not read");
    auto a = App(100, 200), b = App(300, 400, L"C:\\Two\\same.exe"), unknown = App(5, 6, L"");
    b.confidence = Traffic::Confidence::Incomplete;
    unknown.appName = L"PID4321"; unknown.identity = L"unknown:4321:0:1"; unknown.confidence = Traffic::Confidence::Unknown;
    Check(store.Update({ a, b, unknown }) && store.Checkpoint(true), "V2 history update/checkpoint failed");
    Check(Contents(raw) == original, "V2 overwrote legacy raw history before commit");
    Check(store.GetRangeAppTotals(Day()).size() == 4, "Same-name distinct paths or unknown ownership was merged");
    Check(store.GetAllTimeTotal().rxBytes == 415 && store.GetAllTimeTotal().txBytes == 617, "Legacy/new amount merge changed bytes");
    Check(store.AppendEngineMetadata(), "Cannot write the metadata retry fixture");
    ++fakeNow.wMinute;
    Check(store.Update({}), "Minute transition failed");
    Check(Contents(raw).rfind(original, 0) == 0, "V2 rewrote or removed legacy raw records");
    CHistoryTrafficStore recovered;
    recovered.Initialize(fixture.wstring());
    const auto apps = recovered.GetRangeAppTotals(Day());
    Check(apps.size() == 4 && recovered.GetAllTimeTotal().rxBytes == 415 && recovered.GetAllTimeTotal().txBytes == 617,
        "Restart duplicated or lost legacy/V2 history");
    unsigned sameNames{};
    for (const auto& app : apps)
    {
        if (app.appName == L"same.exe") { ++sameNames; Check(!app.identity.empty() && !app.exePath.empty(), "Path identity was lost"); }
        Check(app.appName.rfind(L"v2:", 0) != 0, "Internal identity escaped into the visible program name");
    }
    Check(sameNames == 2, "Visible names were not preserved for same-name paths");
    const auto metadata = recovered.GetEngineMetadata(Day());
    Check(metadata.cutover == a.sampleStart && metadata.sources == 1 && metadata.confidence == 7 && metadata.minutes == 1 && !metadata.corrupt,
        "Cutover, source, quality or retried metadata snapshots changed after restart");
    std::wstring error;
    const auto exported = fixture / L"export.tsv";
    Check(recovered.ExportRange(Day(), exported.wstring(), error), "V2/legacy export failed");
    const auto text = Contents(exported);
    Check(text.find("same.exe") != std::string::npos && text.find("legacy.exe") != std::string::npos && text.find("v2:") == std::string::npos,
        "Export lost real names or exposed internal identities");
    const auto rawBeforeExport = Contents(raw);
    for (const auto& protectedPath : { raw, root / L"apps.tsv", root / L"append.pending", fixture / L"app_traffic_state.tsv",
            fixture / L"history_traffic.dat", fixture / L"network_traffic_history/2026/10/2026-10-16.tsv" })
        Check(!recovered.ExportRange(Day(), protectedPath.wstring(), error), "Export was allowed to overwrite owned history/checkpoints");
    Check(Contents(raw) == rawBeforeExport, "Rejected export changed the raw history file");
    std::ofstream(raw.parent_path() / L"2026-10-16.quality.tsv", std::ios::app) << "10:00\tbroken\n";
    Check(recovered.GetEngineMetadata(Day()).corrupt && recovered.GetAllTimeTotal().rxBytes == 415,
        "Corrupt metadata silently changed traffic or was not diagnosed");
    std::cout << "PASS: legacy preservation, path-separated V2 names, unknown ownership, restart, metadata retry/CRC and export\n";
}
void DamagedDictionaryAndRaw(const std::filesystem::path& fixture)
{
    fakeNow.wHour = 10; fakeNow.wMinute = 0;
    const auto root = fixture / L"app_traffic_history";
    const auto raw = root / L"2026/10/2026-10-16.tsv";
    std::filesystem::create_directories(raw.parent_path());
    const auto dictionary = root / L"apps.tsv";
    const std::string good = "version\t4\napp\t1\tlegacy.exe\n";
    std::ofstream(raw) << "09:00\t1\ta\tb\n";
    std::ofstream(dictionary) << "version\t4\napp\t!\tbroken.exe\n";
    const auto original = Contents(dictionary);
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    store.GetAllTimeTotal();
    Check(!store.IsReady() && !store.Update({ App(7, 9) }) && Contents(dictionary) == original,
        "Damaged dictionary was treated as an empty database or overwritten");
    std::filesystem::remove(dictionary);
    store.GetAllTimeTotal();
    Check(!store.IsReady() && !store.Update({ App(7, 9) }) && !std::filesystem::exists(dictionary),
        "Missing dictionary was silently recreated over existing history IDs");
    std::ofstream(dictionary) << good;
    Check(store.GetRangeTotal(Day()).rxBytes == 10 && store.IsRangeHealthy(Day()) && store.IsAllTimeHealthy(),
        "Restoring a dictionary did not safely reload the original bytes");
    std::ofstream(raw, std::ios::app) << "corrupt\n";
    CHistoryTrafficStore damaged;
    damaged.Initialize(fixture.wstring());
    damaged.GetRangeTotal(Day());
    Check(damaged.IsReady() && !damaged.IsRangeHealthy(Day()) && !damaged.IsAllTimeHealthy() &&
        damaged.GetEngineMetadata(Day()).corrupt,
        "Malformed raw data was presented as complete zero/partial traffic");
    const auto before = Contents(raw);
    std::unordered_map<std::wstring, CHistoryTrafficStore::TrafficAmount> totals{ { L"sentinel", { 3, 5 } } };
    Check(!damaged.AddMinuteFileTotals(raw.wstring(), Day(), totals) && totals.size() == 1 && totals.at(L"sentinel").rxBytes == 3,
        "A failed raw read leaked its partial prefix into fallback totals");
    Check(Contents(raw) == before, "Corrupt-history checks modified the original raw bytes");
    std::cout << "PASS: damaged/missing dictionary blocks unsafe writes; corrupt raw is unknown and fallback reads are atomic\n";
}

void ArchiveFallback(const std::filesystem::path& fixture)
{
    const auto root = fixture / L"app_traffic_history";
    const auto raw = root / L"2026/10/2026-10-15.tsv";
    const auto archive = raw.parent_path() / L"2026-10.tmh";
    std::filesystem::create_directories(raw.parent_path());
    std::ofstream(root / L"apps.tsv") << "version\t4\napp\t1\tlegacy.exe\n";
    std::ofstream(raw) << "09:00\t1\ta\tb\ncorrupt\n";
    const std::vector<unsigned char> payload{ 1, 0x9c, 4, 1, 0, 10, 11 };
    {
        std::ofstream out(archive, std::ios::binary);
        const auto little = [&](std::uint64_t value, unsigned bytes) {
            for (unsigned i = 0; i < bytes; ++i) { out.put(static_cast<char>(value & 255)); value >>= 8; }
        };
        out.write("TMH2", 4); little(2, 2); little(2026, 2); little(10, 1); little(0, 1);
        little(1, 4); little(1, 4);
        out.put(10); out.write("legacy.exe", 10);
        little(15, 1); little(kTmhHeaderSize + 11 + kTmhDayIndexSize, 8); little(payload.size(), 8);
        little(1, 8); little(10, 8); little(11, 8); little(CalculateCrc32(payload.data(), payload.size()), 4);
        out.write(reinterpret_cast<const char*>(payload.data()), payload.size());
    }
    const auto rawBefore = Contents(raw), archiveBefore = Contents(archive);
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    auto range = Day(); range.start.wDay = range.end.wDay = 15;
    range.start.wHour = range.end.wHour = 9; range.start.wMinute = 0; range.end.wMinute = 1;
    const auto amount = store.GetRangeTotal(range);
    Check(amount.rxBytes == 10 && amount.txBytes == 11 && store.IsRangeHealthy(range),
        "TMH fallback duplicated a damaged raw file's valid prefix or lost the archive");
    Check(Contents(raw) == rawBefore && Contents(archive) == archiveBefore, "Archive fallback rewrote history");
    std::cout << "PASS: valid TMH fallback replaces a corrupt raw source without double-counting its prefix\n";
}
void MixedSourceMetadata(const std::filesystem::path& fixture)
{
    fakeNow.wHour = 10; fakeNow.wMinute = 5;
    auto etw = App(101, 202), estats = App(303, 404);
    estats.source = Traffic::Source::EStats;
    estats.confidence = Traffic::Confidence::Incomplete;
    estats.sampleStart = etw.sampleEnd;
    estats.sampleEnd = estats.sampleStart + 10000;
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    Check(store.Update({ etw, estats }) && store.Checkpoint(true), "Mixed-source minute checkpoint failed");
    const auto quality = store.GetEngineMetadata(Day());
    Check(quality.sources == 3 && quality.confidence == 3 && !quality.corrupt,
        "Mixed-source quality lost a source or the worst completeness flag");
    Check(store.GetRangeAppTotals(Day()).size() == 1 && store.GetRangeTotal(Day()).rxBytes == 404 &&
        store.GetRangeTotal(Day()).txBytes == 606, "Mixed sources duplicated a visible app or changed bytes");
    ++fakeNow.wMinute;
    Check(store.Update({}), "Mixed-source minute commit failed");
    CHistoryTrafficStore recovered;
    recovered.Initialize(fixture.wstring());
    Check(recovered.GetEngineMetadata(Day()).sources == 3 && recovered.GetAllTimeTotal().rxBytes == 404 &&
        recovered.GetAllTimeTotal().txBytes == 606 && recovered.GetRangeAppTotals(Day()).size() == 1,
        "Mixed-source provenance or amounts changed across restart");
    std::cout << "PASS: mixed-source same-app amounts and source/confidence metadata survive checkpoint, minute commit and restart\n";
}

void IoFailure(const std::filesystem::path& fixture, bool state_failure)
{
    fakeNow.wHour = 11; fakeNow.wMinute = 0;
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    store.GetAllTimeTotal();
    const auto block = state_failure ? fixture / L"app_traffic_state.tsv.new" : fixture / L"app_traffic_history/current.tmp.new";
    std::filesystem::create_directories(block);
    Check(store.Update({ App(42, 61) }), "Applied RAM deltas were incorrectly reported as unapplied");
    Check(!store.IsWriteHealthy() && store.GetAllTimeTotal().rxBytes == 42, "Write failure lost or duplicated applied traffic");
    std::filesystem::remove(block);
    Check(store.Checkpoint(true), "Idle checkpoint retry did not recover after the file became writable");
    CHistoryTrafficStore recovered;
    recovered.Initialize(fixture.wstring());
    Check(recovered.GetAllTimeTotal().rxBytes == 42 && recovered.GetAllTimeTotal().txBytes == 61,
        "Recovered checkpoint/state replayed an already applied delta");
    std::cout << "PASS: " << (state_failure ? "state" : "checkpoint") << " failure, truthful RAM acknowledgment and idle durable retry\n";
}
void GapOnlyMetadata(const std::filesystem::path& fixture)
{
    fakeNow.wHour = 10; fakeNow.wMinute = 9;
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    auto gap = App(0, 0);
    gap.confidence = Traffic::Confidence::Incomplete;
    Check(store.Update({ gap }) && store.Checkpoint(true), "Zero-byte gap checkpoint failed");
    CHistoryTrafficStore recovered;
    recovered.Initialize(fixture.wstring());
    Check((recovered.GetEngineMetadata(Day()).confidence & 2) && !recovered.GetAllTimeTotal().rxBytes &&
        recovered.GetRangeAppTotals(Day()).empty(), "Gap-only restart lost uncertainty or invented an application amount");
    const auto blocked = fixture / L"app_traffic_history/2026/10/2026-10-16.quality.tsv";
    std::filesystem::create_directories(blocked);
    ++fakeNow.wMinute;
    Check(!recovered.Update({ App(7, 11) }), "Failed gap append consumed a later traffic batch");
    std::filesystem::remove(blocked);
    Check(recovered.Update({ App(7, 11) }) && recovered.Checkpoint(true), "Gap-only append retry failed");
    CHistoryTrafficStore twice;
    twice.Initialize(fixture.wstring());
    Check((twice.GetEngineMetadata(Day()).confidence & 2) && twice.GetAllTimeTotal().rxBytes == 7 &&
        twice.GetAllTimeTotal().txBytes == 11, "Gap commit retry lost provenance or repeated traffic");
    std::cout << "PASS: zero-byte gaps survive checkpoint, restart and append failure without fabricated traffic\n";
}
void MinuteRetry(const std::filesystem::path& fixture, bool metadata_failure)
{
    fakeNow.wHour = 12; fakeNow.wMinute = 0;
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    Check(store.Update({ App(13, 17) }), "Initial minute update failed");
    const auto block = metadata_failure ? fixture / L"app_traffic_history/2026/10/2026-10-16.quality.tsv" :
        fixture / L"app_traffic_history/current.tmp.new";
    std::filesystem::create_directories(block);
    ++fakeNow.wMinute;
    Check(!store.Update({ App(7, 11) }) && store.GetAllTimeTotal().rxBytes == 13,
        "Failed minute commit consumed the next pending batch");
    std::filesystem::remove(block);
    Check(store.Update({ App(7, 11) }) && store.Checkpoint(true), "Retried minute commit failed");
    CHistoryTrafficStore recovered;
    recovered.Initialize(fixture.wstring());
    Check(recovered.GetAllTimeTotal().rxBytes == 20 && recovered.GetAllTimeTotal().txBytes == 28 &&
        recovered.GetRangeTotal(Day()).rxBytes == 20, "Minute/metadata retry duplicated a partially committed block");
    std::cout << "PASS: " << (metadata_failure ? "metadata append" : "minute checkpoint") << " failure/retry without losing or repeating the pending batch\n";
}
void DateParsing()
{
    SYSTEMTIME value{}, last{};
    Check(CHistoryTrafficStore::TryParseStoredTime(L"2024-02-29 23:59", value), "Leap day rejected");
    for (const auto* invalid : { L"2026-02-30 12:00", L"2026-13-01 12:00", L"2026-01-01 24:00", L"2026-01-01 12:60",
            L"2026-1-01 12:00", L"2026-01-01 12:00junk", L"abcd-01-01 12:00" })
        Check(!CHistoryTrafficStore::TryParseStoredTime(invalid, value), "Invalid fixed-format time accepted");
    Check(CHistoryTrafficStore::TryParseBucketKey(L"2026-10-16", value, last) && value.wHour == 0 && last.wHour == 23 && last.wMinute == 59,
        "Legacy day bucket semantics changed");
    std::cout << "PASS: strict fixed dates, leap day, invalid separators/ranges/trailing input and legacy day buckets\n";
}
void DamagedCheckpointMetadata(const std::filesystem::path& fixture)
{
    fakeNow.wHour = 13; fakeNow.wMinute = 0;
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    Check(store.Update({ App(71, 83) }) && store.Checkpoint(true), "Cannot create checkpoint metadata fixture");
    const auto path = fixture / L"app_traffic_history/current.tmp";
    auto text = Contents(path);
    const auto begin = text.find("engine\t");
    Check(begin != std::string::npos, "No engine metadata in checkpoint");
    text.replace(begin, text.find('\n', begin) - begin, "engine\tcorrupt");
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    CHistoryTrafficStore recovered;
    recovered.Initialize(fixture.wstring());
    Check(recovered.GetAllTimeTotal().rxBytes == 71 && recovered.GetAllTimeTotal().txBytes == 83,
        "Damaged provenance changed otherwise valid traffic bytes");
    const auto metadata = recovered.GetEngineMetadata(Day());
    Check(metadata.corrupt && (metadata.confidence & 4), "Damaged provenance was silently presented as complete");
    std::cout << "PASS: corrupt checkpoint provenance preserves bytes and marks source/quality unknown\n";
}
void InterruptedAppend(const std::filesystem::path& fixture, unsigned kind)
{
    fakeNow.wHour = 14; fakeNow.wMinute = 0;
    const auto history = fixture / L"app_traffic_history";
    const auto raw = history / L"2026/10/2026-10-16.tsv";
    std::filesystem::create_directories(raw.parent_path());
    const std::string prefix = "09:00\t1\ta\tb\n";
    std::ofstream(raw, std::ios::binary) << prefix;
    std::ofstream(history / L"apps.tsv") << "version\t4\napp\t1\tlegacy.exe\n";
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    Check(store.Update({ App(100, 200), App(300, 400, L"C:\\Two\\same.exe") }) && store.Checkpoint(true), "Cannot prepare interrupted append checkpoint");
    std::vector<std::pair<std::wstring, CHistoryTrafficStore::TrafficAmount>> entries(store.m_currentMinuteTotals.begin(), store.m_currentMinuteTotals.end());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string block;
    bool first = true;
    for (const auto& entry : entries)
    {
        block += WideToUtf8((first ? L"14:00" : std::wstring{}) + L"\t" + FormatStoredAppId(store.m_appIdByName.at(entry.first)) +
            L"\t" + FormatStoredAppId(entry.second.rxBytes) + L"\t" + FormatStoredAppId(entry.second.txBytes)) + '\n';
        first = false;
    }
    RawAppendHeader header{};
    {
        ManagedFile file;
        file.value = CreateFileW(raw.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        BY_HANDLE_FILE_INFORMATION identity{};
        Check(file.value != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file.value, &identity), "Cannot read fixture file identity");
        header.offset = prefix.size(); header.volume = identity.dwVolumeSerialNumber;
        header.indexHigh = identity.nFileIndexHigh; header.indexLow = identity.nFileIndexLow;
        header.date = (2026u << 16) | (10u << 8) | 16; header.minute = 14 * 60;
        header.allRx = 410; header.allTx = 611;
        header.transaction = store.m_currentCommitId;
        header.cutover = store.m_engine.cutover; header.firstEvent = store.m_currentEngine.firstEvent; header.lastEvent = store.m_currentEngine.lastEvent;
        header.sources = store.m_currentEngine.sources; header.confidence = store.m_currentEngine.confidence;
        Check(AnchorHash(file.value, header.offset, header.anchorHash) && WriteAppendJournal(history.wstring(), header, block), "Cannot persist interrupted append journal");
    }
    auto partial = block.substr(0, block.find('\n') + 4);
    if (kind == 1) partial.back() ^= 1;
    std::ofstream(raw, std::ios::binary | std::ios::app) << partial;
    if (kind == 2) std::ofstream(AppendJournalPath(history.wstring()), std::ios::binary | std::ios::trunc) << "broken";
    const auto before = Contents(raw);
    CHistoryTrafficStore recovered;
    recovered.Initialize(fixture.wstring());
    if (!kind)
    {
        Check(recovered.GetRangeTotal(Day()).rxBytes == 410 && recovered.GetAllTimeTotal().txBytes == 611,
            "Crash recovery duplicated a fully written prefix or lost the rest of a minute");
        Check(Contents(raw) == prefix + block && !std::filesystem::exists(AppendJournalPath(history.wstring())),
            "Crash recovery changed older bytes, repeated rows or left a completed journal");
        CHistoryTrafficStore twice;
        twice.Initialize(fixture.wstring());
        Check(twice.GetRangeTotal(Day()).rxBytes == 410 && Contents(raw) == prefix + block,
            "Reopening a completed append replayed its transaction");
        std::cout << "PASS: partial multi-row append crash recovery resumes exact suffix without rewriting/truncating old history\n";
    }
    else
    {
        recovered.GetAllTimeTotal();
        Check(!recovered.IsReady() && !recovered.IsWriteHealthy() && Contents(raw) == before && !recovered.Update({ App(1, 2) }),
            "Conflicting/corrupt append journal was guessed, overwritten or accepted as a healthy ledger");
        std::wstring error;
        Check(!recovered.ExportRange(Day(), (fixture / L"export.tsv").wstring(), error), "Unavailable history was exported as measured zero");
        const auto state_before = Contents(fixture / L"app_traffic_state.tsv");
        recovered.SetPreferredRange(Day());
        recovered.SetPreferredLanguage(CHistoryTrafficStore::DisplayLanguage::English);
        Check(Contents(fixture / L"app_traffic_state.tsv") == state_before,
            "UI preferences overwrote state while append recovery was blocked");
        std::cout << "PASS: " << (kind == 1 ? "conflicting raw prefix" : "corrupt append journal") << " preserves all existing bytes and blocks unsafe recovery\n";
    }
}
void SameMinuteTransactions(const std::filesystem::path& fixture)
{
    fakeNow.wHour = 15; fakeNow.wMinute = 0;
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    Check(store.Update({ App(100, 200) }), "Cannot prepare same-minute first transaction");
    const auto first_id = store.m_currentCommitId;
    Check(store.FlushCurrentMinute(), "Cannot commit same-minute first transaction");
    const auto raw = fixture / L"app_traffic_history/2026/10/2026-10-16.tsv";
    const auto first_block = Contents(raw);
    Check(store.Update({ App(100, 200) }) && store.m_currentCommitId > first_id && store.Checkpoint(true),
        "A new same-minute batch reused the previous transaction identity");
    CHistoryTrafficStore recovered;
    recovered.Initialize(fixture.wstring());
    Check(recovered.GetRangeTotal(Day()).rxBytes == 200 && recovered.GetAllTimeTotal().txBytes == 400,
        "An earlier same-minute commit discarded legitimate identical new traffic on restart");
    Check(recovered.FlushCurrentMinute() && Contents(raw) == first_block + first_block,
        "An identical tail was mistaken for a replay instead of a distinct transaction");
    ++fakeNow.wMinute;
    Check(recovered.Update({ App(11, 22) }) && recovered.FlushCurrentMinute(), "Cannot prepare forward-clock transaction");
    --fakeNow.wMinute;
    Check(recovered.Update({ App(100, 200) }) && recovered.Checkpoint(true), "Clock rollback batch failed");
    CHistoryTrafficStore rollback;
    rollback.Initialize(fixture.wstring());
    Check(rollback.GetRangeTotal(Day()).rxBytes == 311 && rollback.GetAllTimeTotal().txBytes == 622 && rollback.FlushCurrentMinute(),
        "Clock rollback reused a stale minute commit or duplicated a prior transaction");
    const auto complete = Contents(raw);
    CHistoryTrafficStore twice;
    twice.Initialize(fixture.wstring());
    Check(twice.GetRangeTotal(Day()).rxBytes == 311 && Contents(raw) == complete,
        "Reopening a committed transaction replayed identical bytes");
    std::cout << "PASS: unique same-minute transactions, identical legitimate payloads, clock rollback and restart replay\n";
}
void CommitMarkerFailure(const std::filesystem::path& fixture)
{
    fakeNow.wHour = 16; fakeNow.wMinute = 0;
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    Check(store.Update({ App(13, 17) }), "Cannot prepare failed commit marker");
    const auto blocked = fixture / L"app_traffic_history/current.commit.new";
    std::filesystem::create_directories(blocked);
    ++fakeNow.wMinute;
    Check(!store.Update({ App(7, 11) }), "A failed commit marker consumed the next batch");
    const auto raw = fixture / L"app_traffic_history/2026/10/2026-10-16.tsv";
    const auto appended = Contents(raw);
    Check(!appended.empty() && std::filesystem::exists(fixture / L"app_traffic_history/append.pending"),
        "A failed commit marker lost its durable append intent");
    std::filesystem::remove(blocked);
    CHistoryTrafficStore recovered;
    recovered.Initialize(fixture.wstring());
    Check(recovered.GetRangeTotal(Day()).rxBytes == 13 && Contents(raw) == appended && recovered.Update({ App(7, 11) }) &&
        recovered.Checkpoint(true) && recovered.GetAllTimeTotal().rxBytes == 20,
        "Restart after marker failure repeated appended bytes or lost the unaccepted batch");
    std::cout << "PASS: append completed but commit marker failed; restart resumes marker without repeated traffic\n";
}
void TransactionChecksum(const std::filesystem::path& fixture, bool marker)
{
    fakeNow.wHour = 17; fakeNow.wMinute = 0;
    CHistoryTrafficStore store;
    store.Initialize(fixture.wstring());
    Check(store.Update({ App(13, 17) }) && store.Checkpoint(true), "Cannot prepare transaction checksum fixture");
    const auto checkpoint = fixture / L"app_traffic_history/current.tmp";
    const auto path = marker ? fixture / L"app_traffic_history/current.commit" : checkpoint;
    if (marker)
    {
        const auto id = store.m_currentCommitId;
        Check(store.FlushCurrentMinute(), "Cannot prepare checksummed commit marker");
        Check(WriteCommittedMinute(path.wstring(), L"2026-10-16 17:00", id), "Cannot prepare marker checksum");
        Check(store.Update({ App(13, 17) }) && store.Checkpoint(true), "Cannot prepare later same-minute checkpoint");
    }
    auto text = Contents(path);
    const auto at = marker ? text.find('\t') + 1 : text.find("transaction\t") + 12;
    Check(at < text.size() && text[at] >= '0' && text[at] <= '9', "No transaction ID to damage");
    text[at] = text[at] == '9' ? '8' : static_cast<char>(text[at] + 1);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
    const auto before = Contents(checkpoint);
    CHistoryTrafficStore recovered;
    recovered.Initialize(fixture.wstring());
    recovered.GetAllTimeTotal();
    Check(!recovered.IsReady() && !recovered.IsWriteHealthy() && !recovered.Update({ App(1, 2) }) && Contents(checkpoint) == before,
        "Corrupted transaction metadata was guessed, replayed or silently discarded");
    std::cout << "PASS: " << (marker ? "commit marker" : "checkpoint") << " transaction checksum detects valid-number corruption and preserves pending bytes\n";
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 3 && std::string(argv[1]) == "--reset-fixture")
        {
            const auto fixture = std::filesystem::u8path(argv[2]);
            fakeNow.wDay = 17; fakeNow.wHour = 3; fakeNow.wMinute = 0;
            const auto old = fixture / L"app_traffic_history/2026/10/2026-10-16.tsv";
            const auto before = Contents(old);
            CHistoryTrafficStore reset;
            reset.Initialize(fixture.wstring());
            const auto amount = reset.GetRangeTotal(Day());
            Check(!amount.rxBytes && !amount.txBytes && reset.IsReady() && reset.GetAllTimeTotal().rxBytes == 2963210484269ULL &&
                reset.GetAllTimeTotal().txBytes == 2381640650024ULL, "Reset fixture replayed old today traffic or damaged all-time totals");
            Check(reset.Update({ App(1, 2) }) && reset.Checkpoint(true), "Fresh traffic failed after day reset");
            CHistoryTrafficStore reopened;
            reopened.Initialize(fixture.wstring());
            Check(reopened.GetRangeTotal(Day()).rxBytes == 1 && reopened.GetRangeTotal(Day()).txBytes == 2 &&
                reopened.GetAllTimeTotal().rxBytes == 2963210484270ULL && Contents(old) == before,
                "Fresh day restart restored removed traffic or modified the previous raw day");
            std::cout << "PASS: reset fixture loads as zero today, preserves old cumulative totals and exactly restarts fresh traffic\n";
            return 0;
        }
        const auto root = std::filesystem::path(L"build") / (L"history-recovery-" + std::to_wstring(GetTickCount64()));
        DateParsing();
        IdentityAndLegacy(root / L"identity");
        DamagedDictionaryAndRaw(root / L"dictionary-and-raw");
        ArchiveFallback(root / L"archive-fallback");
        MixedSourceMetadata(root / L"mixed-source");
        GapOnlyMetadata(root / L"gap-only");
        IoFailure(root / L"checkpoint", false);
        IoFailure(root / L"state", true);
        MinuteRetry(root / L"minute", false);
        MinuteRetry(root / L"metadata", true);
        DamagedCheckpointMetadata(root / L"damaged-metadata");
        InterruptedAppend(root / L"append", 0);
        InterruptedAppend(root / L"append-conflict", 1);
        InterruptedAppend(root / L"append-corrupt", 2);
        SameMinuteTransactions(root / L"same-minute");
        CommitMarkerFailure(root / L"commit-marker");
        TransactionChecksum(root / L"checkpoint-checksum", false);
        TransactionChecksum(root / L"marker-checksum", true);
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
