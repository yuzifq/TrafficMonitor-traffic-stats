#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tcpestats.h>
#include "TcpEStatsCollector.h"

#include <cstring>

#pragma comment(lib, "iphlpapi.lib")

using Traffic::U64;
namespace
{
MIB_TCPROW Row4(const Traffic::ConnectionKey& key)
{
    MIB_TCPROW row{};
    std::memcpy(&row.dwLocalAddr, key.local.data(), 4);
    std::memcpy(&row.dwRemoteAddr, key.remote.data(), 4);
    row.dwLocalPort = key.localPort;
    row.dwRemotePort = key.remotePort;
    return row;
}
MIB_TCP6ROW Row6(const Traffic::ConnectionKey& key)
{
    MIB_TCP6ROW row{};
    std::memcpy(&row.LocalAddr, key.local.data(), 16);
    std::memcpy(&row.RemoteAddr, key.remote.data(), 16);
    row.dwLocalScopeId = key.localScope;
    row.dwRemoteScopeId = key.remoteScope;
    row.dwLocalPort = key.localPort;
    row.dwRemotePort = key.remotePort;
    return row;
}
bool Eligible(DWORD state)
{
    return state != MIB_TCP_STATE_CLOSED && state != MIB_TCP_STATE_LISTEN && state != MIB_TCP_STATE_TIME_WAIT &&
        state != MIB_TCP_STATE_DELETE_TCB;
}
}

Traffic::ProcessIdentity CTcpEStatsCollector::QueryOwner(DWORD pid)
{
    Traffic::ProcessIdentity owner;
    owner.pid = pid;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return owner;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user))
        owner.created = (static_cast<U64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    wchar_t path[32768];
    DWORD size = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(process, 0, path, &size))
    {
        owner.path.assign(path, size);
        const auto slash = owner.path.find_last_of(L"\\/");
        owner.name = owner.path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    }
    CloseHandle(process);
    return owner;
}

DWORD CTcpEStatsCollector::Read(const Traffic::ConnectionKey& key, U64& rx, U64& tx, bool& enabled, Cursor& cursor)
{
    TCP_ESTATS_DATA_RW_v0 rw{};
    TCP_ESTATS_DATA_ROD_v0 rod{};
    DWORD error;
    if (key.family == 6)
    {
        auto row = Row6(key);
        error = GetPerTcp6ConnectionEStats(&row, TcpConnectionEstatsData,
            reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), nullptr, 0, 0,
            reinterpret_cast<PUCHAR>(&rod), 0, sizeof(rod));
    }
    else
    {
        auto row = Row4(key);
        error = GetPerTcpConnectionEStats(&row, TcpConnectionEstatsData,
            reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), nullptr, 0, 0,
            reinterpret_cast<PUCHAR>(&rod), 0, sizeof(rod));
    }
    enabled = error == NO_ERROR && rw.EnableCollection != FALSE;
    cursor = {};
    if (enabled)
    {
        rx = rod.DataBytesIn; tx = rod.DataBytesOut;
        cursor.acknowledged = rod.ThruBytesAcked; cursor.received = rod.ThruBytesReceived;
        cursor.sendUnacknowledged = rod.SndUna; cursor.sendNext = rod.SndNxt;
        cursor.sendMaximum = rod.SndMax; cursor.receiveNext = rod.RcvNxt;
    }
    return error;
}

DWORD CTcpEStatsCollector::Enable(const Traffic::ConnectionKey& key, bool enabled)
{
    TCP_ESTATS_DATA_RW_v0 rw{};
    rw.EnableCollection = enabled ? TRUE : FALSE;
    if (key.family == 6)
    {
        auto row = Row6(key);
        return SetPerTcp6ConnectionEStats(&row, TcpConnectionEstatsData, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);
    }
    auto row = Row4(key);
    return SetPerTcpConnectionEStats(&row, TcpConnectionEstatsData, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);
}

void CTcpEStatsCollector::ApplyCounters(State& state, U64 rx, U64 tx, U64 start, U64 end,
    DWORD error, bool enabled, Diagnostics& diagnostics, const Cursor* cursor)
{
    auto& value = state.value;
    value.rxDelta = value.txDelta = 0;
    value.readBeginCounter = value.readEndCounter = 0;
    value.start = start;
    value.startLatest = end;
    value.endEarliest = start;
    value.end = end;
    value.error = error;
    value.enabled = enabled;
    value.baseline = true;
    value.coarse = false;
    if (error != NO_ERROR || !enabled)
    {
        value.cursor = {};
        state.hasBaseline = false;
        ++diagnostics.readFailures;
        diagnostics.lastError = error != NO_ERROR ? error : ERROR_NOT_READY;
        return;
    }
    const bool cursorReset = cursor && (cursor->acknowledged < value.cursor.acknowledged || cursor->received < value.cursor.received);
    if (state.hasBaseline && (rx < value.rx || tx < value.tx || end < state.lastRead || start < state.lastReadStart || cursorReset))
    {
        state.hasBaseline = false;
        ++diagnostics.resets;
    }
    if (state.hasBaseline)
    {
        value.rxDelta = rx - value.rx;
        value.txDelta = tx - value.tx;
        value.start = state.lastReadStart;
        value.startLatest = state.lastRead;
        value.baseline = false;
        value.coarse = end - state.lastReadStart > 50000000ULL;
        if (value.coarse) ++diagnostics.coarseIntervals;
    }
    else ++diagnostics.baselines;
    value.rx = rx;
    value.tx = tx;
    if (cursor) value.cursor = *cursor;
    state.hasBaseline = true;
    state.lastRead = end;
    state.lastReadStart = start;
}

void CTcpEStatsCollector::Visit(const Traffic::ConnectionKey& key, U64 created, bool read_counters)
{
    if (!m_includeLoopback && (Traffic::IsLoopback(key.local, key.family) || Traffic::IsLoopback(key.remote, key.family))) return;
    auto found = m_connections.find(key);
    if (found != m_connections.end() && found->second.value.connectionCreated != created)
    {
        auto old = found->second.value;
        old.closed = true;
        old.rxDelta = old.txDelta = 0;
        m_observations.push_back(std::move(old));
        m_connections.erase(found);
        found = m_connections.end();
    }
    bool fresh = found == m_connections.end();
    if (fresh && (m_stopping || m_connections.size() >= 4096))
    {
        if (!m_stopping) ++m_diagnostics.skipped;
        return;
    }
    if (fresh)
    {
        State state;
        state.value.key = key;
        auto owner = m_owners.find(key.pid);
        if (owner == m_owners.end()) owner = m_owners.emplace(key.pid, QueryOwner(key.pid)).first;
        state.value.owner = owner->second;
        state.value.generation = ++m_generation;
        state.value.connectionCreated = created;
        found = m_connections.emplace(key, std::move(state)).first;
    }
    auto& state = found->second;
    state.seen = m_poll;
    if (m_stopping || (!fresh && !read_counters) || state.retryAfter > GetTickCount64()) return;
    const auto start = Traffic::FileTimeNow();
    LARGE_INTEGER readBegin{}, readEnd{};
    QueryPerformanceCounter(&readBegin);
    U64 rx{}, tx{};
    Cursor cursor;
    bool enabled{};
    auto error = Read(key, rx, tx, enabled, cursor);
    if (error == NO_ERROR && !enabled)
    {
        error = Enable(key, true);
        if (error == NO_ERROR)
        {
            state.enabledByUs = true;
            state.hasBaseline = false;
            error = Read(key, rx, tx, enabled, cursor);
        }
        else ++m_diagnostics.enableFailures;
    }
    QueryPerformanceCounter(&readEnd);
    ApplyCounters(state, rx, tx, start, Traffic::FileTimeNow(), error, enabled, m_diagnostics, &cursor);
    state.value.readBeginCounter = readBegin.QuadPart;
    state.value.readEndCounter = readEnd.QuadPart;
    state.retryAfter = error != NO_ERROR ? GetTickCount64() + (error == ERROR_ACCESS_DENIED ? 60000 : 1000) : 0;
    m_observations.push_back(state.value);
}

void CTcpEStatsCollector::Enumerate(bool ipv6, bool read_counters)
{
    auto& table = ipv6 ? m_table6 : m_table4;
    if (table.empty()) table.resize(65536);
    ULONG size = static_cast<ULONG>(table.size());
    DWORD error = GetExtendedTcpTable(table.data(), &size, FALSE, ipv6 ? AF_INET6 : AF_INET,
        TCP_TABLE_OWNER_MODULE_ALL, 0);
    if (error == ERROR_INSUFFICIENT_BUFFER)
    {
        table.resize(size);
        error = GetExtendedTcpTable(table.data(), &size, FALSE, ipv6 ? AF_INET6 : AF_INET,
            TCP_TABLE_OWNER_MODULE_ALL, 0);
    }
    (ipv6 ? m_diagnostics.tableError6 : m_diagnostics.tableError4) = error;
    if (error != NO_ERROR) return;
    if (ipv6)
    {
        const auto* rows = reinterpret_cast<const MIB_TCP6TABLE_OWNER_MODULE*>(table.data());
        for (DWORD i = 0; i < rows->dwNumEntries; ++i)
        {
            const auto& row = rows->table[i];
            if (!Eligible(row.dwState)) continue;
            Traffic::ConnectionKey key;
            key.family = 6; key.protocol = IPPROTO_TCP; key.pid = row.dwOwningPid;
            std::memcpy(key.local.data(), row.ucLocalAddr, 16);
            std::memcpy(key.remote.data(), row.ucRemoteAddr, 16);
            key.localPort = static_cast<std::uint16_t>(row.dwLocalPort);
            key.remotePort = static_cast<std::uint16_t>(row.dwRemotePort);
            key.localScope = row.dwLocalScopeId; key.remoteScope = row.dwRemoteScopeId;
            Visit(key, row.liCreateTimestamp.QuadPart, read_counters);
        }
    }
    else
    {
        const auto* rows = reinterpret_cast<const MIB_TCPTABLE_OWNER_MODULE*>(table.data());
        for (DWORD i = 0; i < rows->dwNumEntries; ++i)
        {
            const auto& row = rows->table[i];
            if (!Eligible(row.dwState)) continue;
            Traffic::ConnectionKey key;
            key.family = 4; key.protocol = IPPROTO_TCP; key.pid = row.dwOwningPid;
            std::memcpy(key.local.data(), &row.dwLocalAddr, 4);
            std::memcpy(key.remote.data(), &row.dwRemoteAddr, 4);
            key.localPort = static_cast<std::uint16_t>(row.dwLocalPort);
            key.remotePort = static_cast<std::uint16_t>(row.dwRemotePort);
            Visit(key, row.liCreateTimestamp.QuadPart, read_counters);
        }
    }
}

const std::vector<CTcpEStatsCollector::Observation>& CTcpEStatsCollector::Poll(bool read_counters)
{
    LARGE_INTEGER started{}, finished{}, frequency{};
    QueryPerformanceCounter(&started);
    ++m_poll;
    m_owners.clear();
    m_observations.clear();
    m_diagnostics.baselines = 0;
    Enumerate(false, read_counters);
    Enumerate(true, read_counters);
    for (auto it = m_connections.begin(); it != m_connections.end();)
    {
        // A failed table query says nothing about the lifetime of its connections.
        const auto table_error = it->first.family == 6 ? m_diagnostics.tableError6 : m_diagnostics.tableError4;
        if (it->second.seen == m_poll || table_error != NO_ERROR) { ++it; continue; }
        auto value = it->second.value;
        value.closed = true;
        value.rxDelta = value.txDelta = 0;
        value.end = Traffic::FileTimeNow();
        m_observations.push_back(std::move(value));
        ++m_diagnostics.closed;
        it = m_connections.erase(it);
    }
    m_diagnostics.connections = static_cast<std::uint32_t>(m_connections.size());
    m_diagnostics.enabled = 0;
    for (const auto& entry : m_connections) if (entry.second.value.enabled) ++m_diagnostics.enabled;
    ++m_diagnostics.polls;
    QueryPerformanceCounter(&finished);
    QueryPerformanceFrequency(&frequency);
    m_diagnostics.pollMicros += (finished.QuadPart - started.QuadPart) * 1000000ULL / frequency.QuadPart;
    return m_observations;
}

void CTcpEStatsCollector::Stop()
{
    if (m_connections.empty()) return;
    // Refresh identities before touching a global per-connection option. Do not
    // disable statistics on an unrelated connection which reused the tuple.
    m_stopping = true;
    Poll(false);
    for (const auto& entry : m_connections)
        if (entry.second.enabledByUs && entry.second.seen == m_poll) Enable(entry.first, false);
    m_connections.clear();
    m_observations.clear();
    m_stopping = false;
}
