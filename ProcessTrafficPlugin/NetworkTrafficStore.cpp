#include "NetworkTrafficStore.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <string_view>

namespace
{
using U64 = std::uint64_t;

U64 Hash(const char* data, size_t size)
{
    U64 hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) { hash ^= static_cast<unsigned char>(data[i]); hash *= 1099511628211ULL; }
    return hash;
}

bool Number(std::string_view field, U64& value)
{
    const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
    return !field.empty() && parsed.ec == std::errc{} && parsed.ptr == field.data() + field.size();
}

bool AddBytes(U64& target, U64 value)
{
    if (value > (std::numeric_limits<U64>::max)() - target) return false;
    target += value;
    return true;
}

bool ValidId(const std::wstring& id)
{
    return id.size() <= 64 && std::all_of(id.begin(), id.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            c == L'{' || c == L'}' || c == L'-';
    });
}

std::string NarrowId(const std::wstring& id)
{
    std::string result;
    result.reserve(id.size());
    for (const auto c : id) result.push_back(static_cast<char>(c));
    return result;
}

bool AtomicWrite(const std::filesystem::path& path, const char* data, size_t size)
{
    auto temp = path;
    temp += L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{};
    bool ok = size <= MAXDWORD && WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) &&
        written == size && FlushFileBuffers(file);
    if (!CloseHandle(file)) ok = false;
    if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    return ok;
}

SYSTEMTIME UtcTime(U64 milliseconds)
{
    ULARGE_INTEGER value{};
    value.QuadPart = milliseconds * 10000;
    FILETIME file{ value.LowPart, value.HighPart };
    SYSTEMTIME time{};
    FileTimeToSystemTime(&file, &time);
    return time;
}

std::string Timestamp(U64 milliseconds)
{
    const auto t = UtcTime(milliseconds);
    char buffer[40]{};
    sprintf_s(buffer, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return buffer;
}

U64 Coverage(std::vector<std::pair<U64, U64>>& spans)
{
    std::sort(spans.begin(), spans.end());
    U64 total{}, first{}, last{};
    for (const auto& span : spans)
    {
        if (span.first > last) { total += last - first; first = span.first; }
        last = (std::max)(last, span.second);
    }
    return total + last - first;
}
}

std::wstring CNetworkTrafficStore::DayKey(U64 utc_ms)
{
    const auto utc = UtcTime(utc_ms);
    SYSTEMTIME local{};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return {};
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
    return buffer;
}

std::filesystem::path CNetworkTrafficStore::DayPath(const std::wstring& key) const
{
    return m_root / key.substr(0, 4) / key.substr(5, 2) / (key + L".tsv");
}

std::string CNetworkTrafficStore::Encode(const Record& record)
{
    std::string line = std::to_string(record.id) + '\t' + std::to_string(record.startMs) + '\t' +
        std::to_string(record.endMs) + '\t' + std::to_string(record.rxBytes) + '\t' +
        std::to_string(record.txBytes) + '\t' + NarrowId(record.adapterId) + '\t' +
        (record.coarse ? "1" : "0");
    line += '\t' + std::to_string(Hash(line.data(), line.size()));
    return line + '\n';
}

bool CNetworkTrafficStore::Decode(const std::string& line, Record& record)
{
    std::array<std::string_view, 8> fields{};
    size_t start{};
    for (size_t i = 0; i < fields.size(); ++i)
    {
        const auto end = line.find('\t', start);
        if ((i + 1 == fields.size()) != (end == std::string::npos)) return false;
        fields[i] = std::string_view(line).substr(start, end == std::string::npos ? end : end - start);
        start = end + 1;
    }
    U64 flags{}, checksum{};
    if (!Number(fields[0], record.id) || !Number(fields[1], record.startMs) || !Number(fields[2], record.endMs) ||
        !Number(fields[3], record.rxBytes) || !Number(fields[4], record.txBytes) || !Number(fields[6], flags) ||
        !Number(fields[7], checksum) || flags > 1 || record.id == 0 || record.startMs >= record.endMs ||
        record.endMs > 265046774399999ULL || checksum != Hash(line.data(), line.rfind('\t'))) return false;
    record.adapterId.assign(fields[5].begin(), fields[5].end());
    record.coarse = flags != 0;
    return !record.adapterId.empty() && ValidId(record.adapterId);
}

CNetworkTrafficStore::Day CNetworkTrafficStore::Summarize(const std::vector<Record>& records)
{
    Day day;
    std::vector<std::pair<U64, U64>> spans;
    for (const auto& record : records)
    {
        day.first = day.count == 0 ? record.startMs : (std::min)(day.first, record.startMs);
        day.last = (std::max)(day.last, record.endMs);
        day.lastId = (std::max)(day.lastId, record.id);
        day.healthy = AddBytes(day.rx, record.rxBytes) && AddBytes(day.tx, record.txBytes) && day.healthy;
        day.coarse |= record.coarse;
        ++day.count;
        spans.emplace_back(record.startMs, record.endMs);
    }
    day.covered = Coverage(spans);
    return day;
}

void CNetworkTrafficStore::Initialize(const std::wstring& base_dir)
{
    const auto root = std::filesystem::path(base_dir) / L"network_traffic_history";
    if (base_dir.empty() || m_root == root) return;
    if (!m_root.empty()) Flush(true);
    m_root = root;
    m_days.clear();
    m_pending.clear();
    m_current.reset();
    m_cachedDay.clear();
    m_cachedRecords.clear();
    m_nextId = 1;
    m_writeHealthy = true;
    m_checkpointCorrupt = false;
    m_dirty = false;
    m_lastCheckpoint = 0;
    std::error_code error;
    std::filesystem::create_directories(m_root, error);
    if (error) m_writeHealthy = false;
    std::ifstream settings(m_root / L"adapter.txt", std::ios::binary);
    std::string id;
    std::getline(settings, id);
    m_preferredAdapter.assign(id.begin(), id.end());
    if (!ValidId(m_preferredAdapter)) m_preferredAdapter.clear();
    for (const auto& entry : std::filesystem::recursive_directory_iterator(m_root,
        std::filesystem::directory_options::skip_permission_denied, error))
    {
        if (error) { m_writeHealthy = false; break; }
        const auto key = entry.path().stem().wstring();
        if (entry.path().extension() != L".tsv" || key.size() != 10 || key[4] != L'-' || key[7] != L'-' ||
            entry.path().lexically_normal() != DayPath(key).lexically_normal()) continue;
        Day day;
        if (ReadSummary(key, day)) m_days[key] = day;
        else { m_days[key] = {}; RefreshDay(key); }
        m_nextId = (std::max)(m_nextId, m_days[key].lastId + 1);
    }
    RecoverCheckpoint();
    if (!m_pending.empty()) Flush();
}

bool CNetworkTrafficStore::ReadDay(const std::wstring& key, std::vector<Record>& records,
    U64* valid_bytes, bool* repairable) const
{
    records.clear();
    if (valid_bytes) *valid_bytes = 0;
    if (repairable) *repairable = false;
    std::ifstream input(DayPath(key), std::ios::binary);
    std::string line;
    if (!std::getline(input, line) || line != "network\t1" || input.eof())
    {
        if (repairable && input.eof() && line.size() <= 9 && std::string_view("network\t1").substr(0, line.size()) == line)
            *repairable = true;
        return false;
    }
    U64 bytes = line.size() + 1;
    U64 previous_id{};
    while (std::getline(input, line))
    {
        if (input.eof())
        {
            if (valid_bytes) *valid_bytes = bytes;
            if (repairable) *repairable = true;
            return false;
        }
        Record record;
        if (!Decode(line, record) || record.id <= previous_id || DayKey(record.endMs - 1) != key) return false;
        previous_id = record.id;
        records.push_back(std::move(record));
        bytes += line.size() + 1;
    }
    if (valid_bytes) *valid_bytes = bytes;
    return input.eof();
}

bool CNetworkTrafficStore::ReadSummary(const std::wstring& key, Day& day) const
{
    auto path = DayPath(key);
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return false;
    const auto write = std::filesystem::last_write_time(path, error);
    if (error) return false;
    path += L".sum";
    std::array<U64, 13> values{};
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(values.data()), sizeof(values)) || input.peek() != EOF ||
        values[0] != 0x31534d54454eULL || values[1] != size || values[2] != static_cast<U64>(write.time_since_epoch().count()) ||
        values[12] != Hash(reinterpret_cast<const char*>(values.data()), sizeof(values) - sizeof(U64)) ||
        values[11] > 1 || values[3] > values[4] || values[7] > values[4] - values[3]) return false;
    day.first = values[3]; day.last = values[4]; day.rx = values[5]; day.tx = values[6];
    day.covered = values[7]; day.lastId = values[8]; day.count = values[9];
    if (values[10] != 1) return false;
    day.coarse = values[11] != 0;
    day.size = size; day.writeTime = write;
    return true;
}

void CNetworkTrafficStore::SaveSummary(const std::wstring& key, const Day& day) const
{
    if (!day.healthy) return;
    const std::array<U64, 13> values{ 0x31534d54454eULL, day.size,
        static_cast<U64>(day.writeTime.time_since_epoch().count()), day.first, day.last, day.rx, day.tx,
        day.covered, day.lastId, day.count, 1, day.coarse ? 1ULL : 0ULL, 0 };
    auto stored = values;
    stored.back() = Hash(reinterpret_cast<const char*>(stored.data()), sizeof(stored) - sizeof(U64));
    auto path = DayPath(key);
    path += L".sum";
    AtomicWrite(path, reinterpret_cast<const char*>(stored.data()), sizeof(stored));
}

bool CNetworkTrafficStore::RefreshDay(const std::wstring& key) const
{
    auto& day = m_days[key];
    std::error_code error;
    const auto size = std::filesystem::file_size(DayPath(key), error);
    if (error) { day.healthy = false; return false; }
    const auto write = std::filesystem::last_write_time(DayPath(key), error);
    if (error) { day.healthy = false; return false; }
    if (day.healthy && day.size == size && day.writeTime == write) return true;
    std::vector<Record> records;
    const bool valid = ReadDay(key, records);
    day = Summarize(records);
    day.healthy &= valid;
    day.size = size;
    day.writeTime = write;
    if (day.healthy) SaveSummary(key, day);
    m_cachedDay = key;
    m_cachedRecords = std::move(records);
    m_cachedSize = size;
    m_cachedWrite = write;
    return day.healthy;
}

bool CNetworkTrafficStore::SetPreferredAdapter(const std::wstring& id)
{
    if (m_root.empty() || !ValidId(id)) return false;
    const auto text = NarrowId(id);
    if (!AtomicWrite(m_root / L"adapter.txt", text.data(), text.size())) { m_writeHealthy = false; return false; }
    m_preferredAdapter = id;
    return true;
}

bool CNetworkTrafficStore::SaveCheckpoint()
{
    if (m_root.empty() || m_checkpointCorrupt) return false;
    std::string text = "network\t1\n";
    for (const auto& record : m_pending) text += Encode(record);
    if (m_current) text += Encode(*m_current);
    m_writeHealthy = AtomicWrite(m_root / L"pending.tsv", text.data(), text.size());
    if (m_writeHealthy) { m_dirty = false; m_lastCheckpoint = GetTickCount64(); }
    return m_writeHealthy;
}

void CNetworkTrafficStore::RecoverCheckpoint()
{
    std::ifstream input(m_root / L"pending.tsv", std::ios::binary);
    if (!input) return;
    std::string line;
    if (!std::getline(input, line) || line != "network\t1") { m_writeHealthy = false; m_checkpointCorrupt = true; return; }
    U64 previous{};
    while (std::getline(input, line))
    {
        Record record;
        if (input.eof() || !Decode(line, record) || record.id <= previous)
        { m_writeHealthy = false; m_checkpointCorrupt = true; break; }
        previous = record.id;
        m_nextId = (std::max)(m_nextId, record.id + 1);
        m_pending.push_back(std::move(record));
    }
    m_dirty = true;
}

bool CNetworkTrafficStore::Append(const Record& record)
{
    const auto key = DayKey(record.endMs - 1);
    if (key.empty()) return false;
    const auto path = DayPath(key);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    U64 valid_bytes{};
    bool repairable = false;
    std::vector<Record> existing;
    bool committed = false;
    const bool exists = std::filesystem::exists(path, error);
    if (error) return false;
    if (exists)
    {
        const bool valid = ReadDay(key, existing, &valid_bytes, &repairable);
        if (!valid && !repairable) return false;
        if (!existing.empty() && existing.back().id >= record.id)
        {
            const auto previous = std::find_if(existing.begin(), existing.end(), [&](const Record& r) { return r.id == record.id; });
            if (previous == existing.end() || previous->startMs != record.startMs || previous->endMs != record.endMs ||
                previous->rxBytes != record.rxBytes || previous->txBytes != record.txBytes ||
                previous->adapterId != record.adapterId || previous->coarse != record.coarse) return false;
            committed = true;
        }
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(valid_bytes);
    bool ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE;
    if (ok && repairable) ok = SetEndOfFile(file) != FALSE;
    const auto line = committed ? std::string{} : (valid_bytes == 0 ? std::string("network\t1\n") : std::string{}) + Encode(record);
    DWORD written{};
    if (ok && !line.empty()) ok = WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) && written == line.size();
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    if (!CloseHandle(file)) ok = false;
    m_days[key].size = 0;
    const bool refreshed = RefreshDay(key);
    return ok && refreshed;
}

bool CNetworkTrafficStore::Add(const CNetworkTrafficCollector::Interval& interval)
{
    if (m_root.empty() || interval.startMs >= interval.endMs || !ValidId(interval.adapterId) || interval.adapterId.empty()) return false;
    const bool merge = m_current && !m_current->coarse && !interval.coarse &&
        m_current->adapterId == interval.adapterId && m_current->endMs == interval.startMs &&
        m_current->startMs / 60000 == interval.startMs / 60000 && interval.startMs / 60000 == (interval.endMs - 1) / 60000;
    if (merge && interval.rxBytes <= (std::numeric_limits<U64>::max)() - m_current->rxBytes &&
        interval.txBytes <= (std::numeric_limits<U64>::max)() - m_current->txBytes)
    {
        m_current->endMs = interval.endMs;
        m_current->rxBytes += interval.rxBytes;
        m_current->txBytes += interval.txBytes;
    }
    else
    {
        if (m_current) m_pending.push_back(std::move(*m_current));
        Record record;
        static_cast<CNetworkTrafficCollector::Interval&>(record) = interval;
        record.id = m_nextId++;
        m_current = std::move(record);
    }
    m_dirty = true;
    if (!m_pending.empty()) return Flush();
    if (GetTickCount64() - m_lastCheckpoint >= 10000) return SaveCheckpoint();
    return m_writeHealthy;
}

bool CNetworkTrafficStore::Flush(bool finish_current)
{
    if (m_root.empty() || m_checkpointCorrupt) return false;
    if (finish_current && m_current) { m_pending.push_back(std::move(*m_current)); m_current.reset(); m_dirty = true; }
    if ((m_dirty || !m_pending.empty()) && !SaveCheckpoint()) return false;
    while (!m_pending.empty())
    {
        if (!Append(m_pending.front())) { m_writeHealthy = false; return false; }
        m_pending.pop_front();
        m_dirty = true;
    }
    return !m_dirty || SaveCheckpoint();
}

bool CNetworkTrafficStore::RangeBounds(const Range& range, U64& start_ms, U64& end_ms)
{
    const auto convert = [](SYSTEMTIME time, U64& result) {
        time.wSecond = time.wMilliseconds = 0;
        SYSTEMTIME utc{};
        FILETIME file{};
        if (!TzSpecificLocalTimeToSystemTime(nullptr, &time, &utc) || !SystemTimeToFileTime(&utc, &file)) return false;
        result = (static_cast<U64>(file.dwHighDateTime) << 32 | file.dwLowDateTime) / 10000;
        return true;
    };
    if (!convert(range.start, start_ms) || !convert(range.end, end_ms)) return false;
    if (start_ms > end_ms) std::swap(start_ms, end_ms);
    end_ms += 60000;
    return true;
}

CNetworkTrafficStore::Query CNetworkTrafficStore::GetRange(const Range& range) const
{
    U64 start{}, end{};
    if (!RangeBounds(range, start, end)) { Query result; result.healthy = false; return result; }
    return GetRangeUtc(start, (std::min)(end, CNetworkTrafficCollector::UtcNowMs()));
}

CNetworkTrafficStore::Query CNetworkTrafficStore::GetRangeUtc(U64 start, U64 end) const
{
    Query result;
    result.healthy = m_writeHealthy;
    if (start >= end) return result;
    result.requestedMs = end - start;
    std::vector<std::pair<U64, U64>> spans;
    const auto add_record = [&](const Record& record) {
        if (record.endMs <= start || record.startMs >= end) return;
        result.available = true;
        result.coarse |= record.coarse;
        auto& amount = record.startMs >= start && record.endMs <= end ? result.amount : result.boundary;
        result.healthy = AddBytes(amount.rxBytes, record.rxBytes) && AddBytes(amount.txBytes, record.txBytes) && result.healthy;
        spans.emplace_back((std::max)(start, record.startMs), (std::min)(end, record.endMs));
    };
    for (auto& pair : m_days)
    {
        const auto& key = pair.first;
        if (pair.second.healthy && (pair.second.last <= start || pair.second.first >= end)) continue;
        RefreshDay(key);
        const auto& day = pair.second;
        result.healthy &= day.healthy;
        if (day.last <= start || day.first >= end) continue;
        const bool pending_day = (m_current && DayKey(m_current->endMs - 1) == key) ||
            std::any_of(m_pending.begin(), m_pending.end(), [&](const Record& r) { return DayKey(r.endMs - 1) == key; });
        if (day.healthy && day.first >= start && day.last <= end && !pending_day)
        {
            result.available |= day.count != 0;
            result.healthy = AddBytes(result.amount.rxBytes, day.rx) && AddBytes(result.amount.txBytes, day.tx) && result.healthy;
            result.coveredMs += day.covered;
            result.coarse |= day.coarse;
            continue;
        }
        if (m_cachedDay != key || m_cachedSize != day.size || m_cachedWrite != day.writeTime)
        {
            result.healthy &= ReadDay(key, m_cachedRecords);
            m_cachedDay = key; m_cachedSize = day.size; m_cachedWrite = day.writeTime;
        }
        for (const auto& record : m_cachedRecords) add_record(record);
    }
    for (const auto& record : m_pending)
    {
        const auto committed = m_days.find(DayKey(record.endMs - 1));
        if (committed == m_days.end() || committed->second.lastId < record.id) add_record(record);
    }
    if (m_current) add_record(*m_current);
    result.coveredMs = (std::min)(result.requestedMs, result.coveredMs + Coverage(spans));
    return result;
}

CNetworkTrafficStore::Amount CNetworkTrafficStore::GetAllTimeTotal(bool* healthy) const
{
    Amount result{};
    bool valid = m_writeHealthy;
    for (const auto& pair : m_days)
        valid = AddBytes(result.rxBytes, pair.second.rx) && AddBytes(result.txBytes, pair.second.tx) && pair.second.healthy && valid;
    for (const auto& record : m_pending)
    {
        const auto committed = m_days.find(DayKey(record.endMs - 1));
        if (committed == m_days.end() || committed->second.lastId < record.id)
            valid = AddBytes(result.rxBytes, record.rxBytes) && AddBytes(result.txBytes, record.txBytes) && valid;
    }
    if (m_current) valid = AddBytes(result.rxBytes, m_current->rxBytes) && AddBytes(result.txBytes, m_current->txBytes) && valid;
    if (healthy) *healthy = valid;
    return result;
}

bool CNetworkTrafficStore::IsManagedPath(const std::wstring& path) const
{
    if (path.empty() || m_root.empty()) return false;
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(m_root.parent_path(), error);
    if (error) return true;
    const auto target = std::filesystem::weakly_canonical(std::filesystem::path(path), error);
    if (error) return true;
    auto prefix = root.wstring();
    if (prefix.back() != L'\\') prefix += L'\\';
    const auto full = target.wstring();
    if (full.size() < prefix.size() || _wcsnicmp(full.c_str(), prefix.c_str(), prefix.size()) != 0) return false;
    const auto relative = full.substr(prefix.size());
    const auto first = relative.substr(0, relative.find(L'\\'));
    return _wcsicmp(first.c_str(), L"network_traffic_history") == 0 ||
        _wcsicmp(first.c_str(), L"app_traffic_history") == 0 ||
        _wcsnicmp(first.c_str(), L"app_traffic_state.tsv", 21) == 0 ||
        _wcsnicmp(first.c_str(), L"history_traffic.dat", 19) == 0;
}

bool CNetworkTrafficStore::ExportRange(const Range& range, const std::wstring& output_path, std::wstring& error) const
{
    error.clear();
    U64 start{}, end{};
    if (!RangeBounds(range, start, end) || output_path.empty() || IsManagedPath(output_path))
    { error = L"Invalid range or protected history path."; return false; }
    end = (std::min)(end, CNetworkTrafficCollector::UtcNowMs());
    auto temporary = std::filesystem::path(output_path);
    temporary += L".tmp";
    if (IsManagedPath(temporary.wstring())) { error = L"Protected history path."; return false; }
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) { error = L"Cannot create export file."; return false; }
    output << "StartUtc\tEndUtc\tDownloadBytes\tUploadBytes\tAdapterId\tPrecision\tRangeRelation\n";
    const auto write = [&](const Record& record) {
        if (record.endMs <= start || record.startMs >= end) return;
        output << Timestamp(record.startMs) << '\t' << Timestamp(record.endMs) << '\t' << record.rxBytes << '\t' <<
            record.txBytes << '\t' << NarrowId(record.adapterId) << '\t' <<
            (record.coarse ? "coarse" : "sampled") << '\t' <<
            (record.startMs >= start && record.endMs <= end ? "inside" : "boundary") << '\n';
    };
    for (auto& pair : m_days)
    {
        if (pair.second.healthy && (pair.second.last <= start || pair.second.first >= end)) continue;
        if (!RefreshDay(pair.first)) { error = L"A network history file is incomplete or damaged."; return false; }
        if (pair.second.last <= start || pair.second.first >= end) continue;
        std::vector<Record> records;
        if (!ReadDay(pair.first, records)) { error = L"Cannot read network history."; return false; }
        for (const auto& record : records) write(record);
    }
    for (const auto& record : m_pending)
    {
        const auto committed = m_days.find(DayKey(record.endMs - 1));
        if (committed == m_days.end() || committed->second.lastId < record.id) write(record);
    }
    if (m_current) write(*m_current);
    output.flush();
    const bool good = output.good();
    output.close();
    if (!good || output.fail() || !MoveFileExW(temporary.c_str(), output_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    { error = L"Cannot finish network history export."; return false; }
    return true;
}
