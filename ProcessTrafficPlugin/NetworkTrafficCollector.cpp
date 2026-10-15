#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <objbase.h>

#include "NetworkTrafficCollector.h"

#include <algorithm>
#include <memory>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ole32.lib")

std::uint64_t CNetworkTrafficCollector::UtcNowMs()
{
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32 | time.dwLowDateTime) / 10000;
}

void CNetworkTrafficCollector::SelectAdapter(const std::wstring& id)
{
    if (m_snapshot.preferredId == id) return;
    m_snapshot.preferredId = id;
    m_snapshot.activeName.clear();
    m_snapshot.rxPerSec = m_snapshot.txPerSec = 0;
    m_snapshot.status = Status::Starting;
    m_hasBaseline = false;
    m_idleSince = 0;
}

std::optional<CNetworkTrafficCollector::Interval> CNetworkTrafficCollector::Sample()
{
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR)
    {
        m_hasBaseline = false;
        m_snapshot.status = Status::Error;
        m_snapshot.rxPerSec = m_snapshot.txPerSec = 0;
        return std::nullopt;
    }
    const std::unique_ptr<MIB_IF_TABLE2, decltype(&FreeMibTable)> owned(table, FreeMibTable);
    std::vector<Adapter> adapters;
    adapters.reserve(table->NumEntries);
    for (ULONG i = 0; i < table->NumEntries; ++i)
    {
        const auto& row = table->Table[i];
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.InterfaceAndOperStatusFlags.FilterInterface) continue;
        wchar_t id[40]{};
        if (StringFromGUID2(row.InterfaceGuid, id, static_cast<int>(std::size(id))) == 0) continue;
        Adapter adapter;
        adapter.id = id;
        adapter.name = row.Alias[0] != L'\0' ? row.Alias : row.Description;
        adapter.instance = row.InterfaceLuid.Value;
        adapter.rxBytes = row.InOctets;
        adapter.txBytes = row.OutOctets;
        adapter.connected = row.OperStatus == IfOperStatusUp;
        adapter.hardware = row.InterfaceAndOperStatusFlags.HardwareInterface != 0;
        adapters.push_back(std::move(adapter));
    }
    return Observe(std::move(adapters), GetTickCount64(), UtcNowMs());
}

std::optional<CNetworkTrafficCollector::Interval> CNetworkTrafficCollector::Observe(
    std::vector<Adapter> adapters, std::uint64_t tick, std::uint64_t utc_ms)
{
    std::sort(adapters.begin(), adapters.end(), [](const Adapter& a, const Adapter& b) { return a.id < b.id; });
    m_snapshot.adapters = std::move(adapters);
    m_snapshot.rxPerSec = m_snapshot.txPerSec = 0;
    const Adapter* selected = nullptr;
    if (!m_snapshot.preferredId.empty())
    {
        for (const auto& adapter : m_snapshot.adapters)
            if (adapter.id == m_snapshot.preferredId) selected = &adapter;
    }
    else
    {
        // Keep a working choice stable; reconsider it after 30 seconds without traffic.
        const bool has_hardware = std::any_of(m_snapshot.adapters.begin(), m_snapshot.adapters.end(),
            [](const Adapter& adapter) { return adapter.connected && adapter.hardware; });
        for (const auto& adapter : m_snapshot.adapters)
            if (adapter.id == m_previous.id && adapter.connected && (!has_hardware || adapter.hardware)) selected = &adapter;
        if (selected == nullptr || (m_idleSince != 0 && tick - m_idleSince >= 30000))
        {
            selected = nullptr;
            for (const auto& adapter : m_snapshot.adapters)
            {
                if (!adapter.connected || (has_hardware && !adapter.hardware)) continue;
                if (selected == nullptr || static_cast<long double>(adapter.rxBytes) + adapter.txBytes >
                    static_cast<long double>(selected->rxBytes) + selected->txBytes) selected = &adapter;
            }
            m_idleSince = 0;
        }
    }
    if (selected == nullptr || !selected->connected)
    {
        m_snapshot.activeName = selected != nullptr ? selected->name : L"";
        m_snapshot.status = Status::Disconnected;
        m_hasBaseline = false;
        return std::nullopt;
    }
    m_snapshot.activeName = selected->name;
    if (m_hasBaseline && selected->id == m_previous.id && selected->instance == m_previous.instance &&
        (tick == m_previousTick || utc_ms == m_previousUtc)) return std::nullopt;
    std::optional<Interval> result;
    if (m_hasBaseline && selected->id == m_previous.id && selected->instance == m_previous.instance &&
        tick > m_previousTick && utc_ms > m_previousUtc &&
        selected->rxBytes >= m_previous.rxBytes && selected->txBytes >= m_previous.txBytes)
    {
        const auto elapsed = tick - m_previousTick;
        const auto wall_elapsed = utc_ms - m_previousUtc;
        const auto drift = elapsed > wall_elapsed ? elapsed - wall_elapsed : wall_elapsed - elapsed;
        if (drift <= 2000)
        {
            result = Interval{ m_previousUtc, utc_ms, selected->rxBytes - m_previous.rxBytes,
                selected->txBytes - m_previous.txBytes, selected->id, elapsed > 5000 };
            m_snapshot.rxPerSec = static_cast<std::uint64_t>(static_cast<long double>(result->rxBytes) * 1000 / elapsed);
            m_snapshot.txPerSec = static_cast<std::uint64_t>(static_cast<long double>(result->txBytes) * 1000 / elapsed);
            if (result->rxBytes == 0 && result->txBytes == 0)
            {
                if (m_idleSince == 0) m_idleSince = tick;
            }
            else m_idleSince = 0;
        }
    }
    m_previous = *selected;
    m_previousTick = tick;
    m_previousUtc = utc_ms;
    m_hasBaseline = true;
    m_snapshot.status = result.has_value() ? Status::Running : Status::Starting;
    return result;
}
