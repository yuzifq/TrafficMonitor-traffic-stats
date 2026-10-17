#include "TcpSourceTracker.h"
#include <algorithm>
#include <limits>

using Traffic::U64;

void CTcpSourceTracker::Reset()
{
    m_connections.clear(); m_aliases.clear(); m_windows.clear(); m_legacy.clear(); m_slices.clear();
    m_generation = 0; m_windowCount = 0; m_healthy = m_legacyHealthy = false; m_legacyClock = 0; m_diagnostics = {};
}
void CTcpSourceTracker::SetHealth(bool healthy, ULONG legacyClock, bool legacyHealthy)
{
    m_healthy = healthy; m_legacyClock = legacyClock; m_legacyHealthy = legacyHealthy;
    if (!healthy || legacyClock != 1)
        for (auto& entry : m_connections) if (entry.second.switched && !entry.second.frozen)
        {
            entry.second.frozen = true;
            if (entry.second.state != Traffic::ConnectionState::EStatsActive)
            {
                ++m_diagnostics.gaps;
                Emit(entry.second, 0, 0, entry.second.lastUtc, Traffic::FileTimeNow(), Traffic::Source::Etw, Traffic::Confidence::Incomplete);
            }
            Fallback(entry.second, Traffic::FileTimeNow());
        }
}
bool CTcpSourceTracker::Add(std::vector<Span>& spans, U64 left, U64 right, U64& added)
{
    added = 0;
    if (left >= right) return true;
    U64 overlap{};
    auto first = spans.begin();
    while (first != spans.end() && first->second < left) ++first;
    auto last = first;
    U64 mergedLeft = left, mergedRight = right;
    while (last != spans.end() && last->first <= mergedRight)
    {
        const auto a = (std::max)(left, last->first), b = (std::min)(right, last->second);
        if (b > a) overlap += b - a;
        mergedLeft = (std::min)(mergedLeft, last->first); mergedRight = (std::max)(mergedRight, last->second);
        ++last;
    }
    if (first == last && spans.size() >= 64) return false;
    const auto at = spans.erase(first, last);
    spans.insert(at, { mergedLeft, mergedRight });
    added = right - left - overlap;
    return true;
}
bool CTcpSourceTracker::Credit(Direction& direction, U64 left, U64 right, U64& bytes)
{
    left = (std::max)(left, direction.prefix);
    if (!Add(direction.credited, left, right, bytes)) return false;
    while (!direction.credited.empty() && direction.credited.front().first <= direction.prefix)
    {
        direction.prefix = (std::max)(direction.prefix, direction.credited.front().second);
        direction.credited.erase(direction.credited.begin());
    }
    return true;
}
bool CTcpSourceTracker::Position(std::uint32_t sequence, std::uint32_t origin, U64 prefix, U64& position)
{
    constexpr U64 cycle = 1ULL << 32, half = cycle / 2;
    position = (prefix & ~(cycle - 1)) + static_cast<std::uint32_t>(sequence - origin);
    if (position > prefix && position - prefix > half)
    {
        if (position < cycle) return false;
        position -= cycle;
    }
    else if (prefix > position && prefix - position > half)
    {
        if (position > (std::numeric_limits<U64>::max)() - cycle) return false;
        position += cycle;
    }
    return (position > prefix ? position - prefix : prefix - position) < half;
}
void CTcpSourceTracker::CloseWindows(const Traffic::ConnectionKey& key, U64 counter)
{
    const auto found = m_windows.find(key);
    if (found == m_windows.end()) return;
    for (auto& window : found->second)
        if (!window.end && counter > window.begin) window.end = counter;
}
bool CTcpSourceTracker::Suppressed(const Traffic::NetworkEvent& event) const
{
    const auto found = m_windows.find(event.key);
    if (found == m_windows.end()) return false;
    for (const auto& window : found->second)
        if (event.clockType != 1 || !event.rawTimestamp ||
            (event.rawTimestamp >= window.begin && (!window.end || event.rawTimestamp < window.end))) return true;
    return false;
}
bool CTcpSourceTracker::KeepLegacy(const Traffic::NetworkEvent& event)
{
    if (event.key.protocol != 6 || !event.hasEndpoints) return true;
    if (event.lifecycle && (event.opcode == 12 || event.opcode == 15 || event.opcode == 28 || event.opcode == 31))
    {
        if (event.clockType == 1 && event.rawTimestamp)
        {
            if (m_legacy.size() < 4096 || m_legacy.find(event.key) != m_legacy.end())
            {
                auto& marker = m_legacy[event.key];
                if (event.rawTimestamp > marker.begin)
                {
                    if (marker.begin)
                    {
                        const auto alias = m_aliases.find(event.key);
                        if (alias != m_aliases.end())
                        {
                            auto& prior = m_connections.at(alias->second);
                            prior.eligible = false;
                            Fallback(prior, event.timestamp);
                            prior.closed = event.rawTimestamp;
                        }
                        CloseWindows(event.key, event.rawTimestamp);
                    }
                    marker = {}; marker.begin = event.rawTimestamp; marker.tick = GetTickCount64();
                }
            }
        }
        return true;
    }
    if (!event.bytes || event.retransmit) return true;
    if (Suppressed(event))
    {
        (event.send ? m_diagnostics.suppressedTx : m_diagnostics.suppressedRx) += event.bytes;
        return false;
    }
    if (m_legacy.size() < 4096 || m_legacy.find(event.key) != m_legacy.end())
    {
        auto& value = m_legacy[event.key];
        value.counter = (std::max)(value.counter, event.rawTimestamp);
        value.unmapped |= event.clockType != 1 || !event.rawTimestamp;
        (event.send ? value.tx : value.rx) += event.bytes;
        value.tick = GetTickCount64();
    }
    else { m_healthy = false; ++m_diagnostics.overflows; }
    return true;
}
void CTcpSourceTracker::Emit(Connection& c, U64 rx, U64 tx, U64 start, U64 end,
    Traffic::Source source, Traffic::Confidence confidence)
{
    auto& slice = m_slices[{ c.generation, source }];
    if (!slice.generation)
    {
        slice.key = c.key; slice.owner = c.owner; slice.generation = c.generation;
        slice.start = start; slice.source = source;
    }
    slice.rx += rx; slice.tx += tx; slice.start = (std::min)(slice.start, start);
    slice.end = (std::max)(slice.end, (std::max)(start, end)); slice.confidence = (std::max)(slice.confidence, confidence);
}
void CTcpSourceTracker::Fallback(Connection& c, U64 utc)
{
    if (!c.switched) { c.state = Traffic::ConnectionState::Uncertain; c.probeEnd = 0; return; }
    if (c.state == Traffic::ConnectionState::EStatsActive)
    {
        ++m_diagnostics.fallbacks; ++m_diagnostics.gaps;
        Emit(c, 0, 0, c.lastUtc, (std::max)(utc, c.lastUtc), Traffic::Source::Etw, Traffic::Confidence::Incomplete);
    }
    c.state = Traffic::ConnectionState::Uncertain;
    Flush(c);
}
void CTcpSourceTracker::Flush(Connection& c)
{
    if (!c.switched || c.state == Traffic::ConnectionState::EStatsActive || c.frozen) return;
    U64 rx{}, tx{};
    for (auto* direction : { &c.rx, &c.tx })
    {
        U64 safe = (std::numeric_limits<U64>::max)();
        if (direction == &c.rx)
        {
            // Only the terminal sequence-space octet can still be FIN. Keep
            // it uncertain until a counter/FIN observation proves its role.
            safe = c.haveFin ? c.rxFin : (std::max)(direction->confirmed, direction->highest ? direction->highest - 1 : 0);
        }
        std::vector<Span> remaining;
        for (const auto& span : direction->pending)
        {
            U64 bytes{};
            if (!Credit(*direction, span.first, (std::min)(span.second, safe), bytes))
            {
                c.frozen = true; ++m_diagnostics.overflows; ++m_diagnostics.gaps;
                break;
            }
            (direction == &c.rx ? rx : tx) += bytes;
            if (span.second > safe && (!c.haveFin || direction != &c.rx)) remaining.emplace_back((std::max)(span.first, safe), span.second);
        }
        direction->pending.swap(remaining);
    }
    if (rx || tx) Emit(c, rx, tx, c.lastUtc, c.utc, Traffic::Source::Etw, Traffic::Confidence::Incomplete);
}
void CTcpSourceTracker::OnProof(const CTcpProofCollector::Event& event)
{
    using Kind = CTcpProofCollector::Kind;
    if (!event.tcb || !event.counter) return;
    auto found = m_connections.find(event.tcb);
    if (event.kind == Kind::Connect && event.endpoints)
    {
        const auto oldAlias = m_aliases.find(event.key);
        if (oldAlias != m_aliases.end())
        {
            auto& old = m_connections.at(oldAlias->second);
            if (event.counter <= old.begin) return;
            Fallback(old, event.timestamp); old.closed = event.counter;
        }
        CloseWindows(event.key, event.counter);
        const auto legacy = m_legacy.find(event.key);
        if (legacy != m_legacy.end() && legacy->second.begin < event.counter) m_legacy.erase(legacy);
        if (found != m_connections.end())
        {
            Fallback(found->second, event.timestamp);
            const auto alias = m_aliases.find(found->second.key);
            if (alias != m_aliases.end() && alias->second == event.tcb) m_aliases.erase(alias);
            m_connections.erase(found);
        }
        if (m_connections.size() >= 4096) { ++m_diagnostics.overflows; return; }
        Connection c;
        c.key = event.key; c.tcb = event.tcb; c.generation = ++m_generation;
        c.begin = event.counter; c.bornUtc = event.timestamp; c.utc = event.timestamp; c.lastUtc = event.timestamp; c.tick = GetTickCount64();
        m_aliases[event.key] = event.tcb;
        found = m_connections.emplace(event.tcb, std::move(c)).first;
    }
    if (found == m_connections.end()) return;
    auto& c = found->second;
    if (event.counter < c.begin) return;
    c.tick = GetTickCount64(); c.utc = (std::max)(c.utc, event.timestamp);
    if (event.kind == Kind::Established)
    {
        if (!event.endpoints || !(event.key == c.key)) { c.eligible = false; Fallback(c, event.timestamp); return; }
        c.connected = true; c.established = event.counter;
    }
    else if (event.kind == Kind::Syn)
    {
        if (c.haveSend && c.sendOrigin != event.sequence + 1) { c.eligible = false; Fallback(c, event.timestamp); }
        else { c.sendOrigin = event.sequence + 1; c.haveSend = true; }
    }
    else if (event.kind == Kind::Receive && !c.connected)
    {
        if (event.bytes != 1 || (c.haveReceive && c.receiveOrigin != event.sequence)) c.eligible = false;
        else { c.receiveOrigin = event.sequence; c.haveReceive = true; }
    }
    else if (event.kind == Kind::Close)
    {
        c.closed = event.counter; Fallback(c, event.timestamp);
    }
    else if (event.kind == Kind::Fin && c.haveReceive)
    {
        U64 position{};
        if (!Position(event.sequence, c.receiveOrigin, c.rx.prefix, position) || !position || position - 1 < c.rx.prefix)
        { c.frozen = true; c.eligible = false; ++m_diagnostics.gaps; return; }
        c.haveFin = true; c.rxFin = position - 1;
        Fallback(c, event.timestamp);
    }
    else if (event.kind == Kind::Send || event.kind == Kind::Receive)
    {
        if ((c.closed && event.counter >= c.closed) || !event.bytes || c.frozen) return;
        const bool send = event.kind == Kind::Send;
        auto& direction = send ? c.tx : c.rx;
        U64 left{}, ignored{};
        if (!(send ? c.haveSend : c.haveReceive) || event.bytes >= (1ULL << 31) ||
            !Position(event.sequence, send ? c.sendOrigin : c.receiveOrigin, c.switched ? direction.prefix : direction.seenPrefix, left) ||
            left > (std::numeric_limits<U64>::max)() - event.bytes)
        {
            c.frozen = true; c.eligible = false; ++m_diagnostics.overflows; Fallback(c, event.timestamp); return;
        }
        direction.highest = (std::max)(direction.highest, left + event.bytes);
        if (!c.switched)
        {
            Direction seen;
            seen.prefix = direction.seenPrefix; seen.credited.swap(direction.seen);
            if (!Credit(seen, left, left + event.bytes, ignored)) { c.eligible = false; ++m_diagnostics.overflows; }
            direction.seenPrefix = seen.prefix; direction.seen.swap(seen.credited);
            return;
        }
        if (!Add(direction.pending, (std::max)(left, direction.prefix), left + event.bytes, ignored))
        { c.frozen = true; ++m_diagnostics.overflows; Fallback(c, event.timestamp); return; }
        Flush(c);
    }
}
bool CTcpSourceTracker::SameOwner(const Connection& c, const CTcpEStatsCollector::Observation& sample) const
{
    return sample.owner.pid == c.key.pid && sample.owner.created && sample.owner.created <= c.bornUtc && !sample.owner.path.empty() &&
        (!c.owner.created || c.owner.created == sample.owner.created) &&
        (!c.estatsGeneration || c.estatsGeneration == sample.generation) &&
        sample.connectionCreated >= sample.owner.created && sample.connectionCreated <= c.bornUtc &&
        (!c.contextCreated || c.contextCreated == sample.connectionCreated);
}
void CTcpSourceTracker::Observe(const CTcpEStatsCollector::Observation& sample)
{
    const auto alias = m_aliases.find(sample.key);
    if (alias == m_aliases.end()) return;
    auto& c = m_connections.at(alias->second);
    c.tick = GetTickCount64();
    const bool identity = SameOwner(c, sample);
    if (!identity || sample.closed || sample.error || !sample.enabled || sample.coarse ||
        !sample.readBeginCounter || sample.readEndCounter < sample.readBeginCounter)
    { Fallback(c, sample.end); return; }
    if (!c.owner.created) c.owner = sample.owner;
    c.estatsGeneration = sample.generation; c.contextCreated = sample.connectionCreated;
    const auto& cursor = sample.cursor;
    const auto maximum = (std::numeric_limits<U64>::max)();
    if (cursor.received > maximum - c.rxThroughOffset || cursor.acknowledged > maximum - c.txThroughOffset ||
        cursor.acknowledged + c.txThroughOffset > maximum - static_cast<std::uint32_t>(cursor.sendMaximum - cursor.sendUnacknowledged))
    { ++m_diagnostics.rejected; Fallback(c, sample.end); return; }
    const auto received = cursor.received + c.rxThroughOffset;
    const bool receivePosition = static_cast<std::uint32_t>(cursor.receiveNext - c.receiveOrigin) ==
        static_cast<std::uint32_t>(received + (c.haveFin ? 1 : 0));
    if (c.switched && receivePosition && received >= c.rx.prefix)
        c.rx.confirmed = (std::max)(c.rx.confirmed, received);
    if (c.switched)
    {
        if (c.state != Traffic::ConnectionState::EStatsActive) { Flush(c); return; }
        const auto inFlight = static_cast<std::uint32_t>(cursor.sendMaximum - cursor.sendUnacknowledged);
        const auto acknowledged = cursor.acknowledged + c.txThroughOffset;
        const auto tx = acknowledged + inFlight, rx = received;
        const bool valid = m_healthy && m_legacyClock == 1 && !sample.baseline && !c.closed && !c.haveFin &&
            sample.readBeginCounter > c.readEnd && inFlight < (1u << 31) &&
            static_cast<std::uint32_t>(cursor.sendUnacknowledged - c.sendOrigin) == static_cast<std::uint32_t>(acknowledged) &&
            receivePosition && rx >= c.rx.prefix && tx >= c.tx.prefix && sample.rx >= c.lastDataRx && sample.tx >= c.lastDataTx &&
            sample.rx - c.lastDataRx == rx - c.rx.prefix && sample.tx - c.lastDataTx == tx - c.tx.prefix;
        if (!valid) { ++m_diagnostics.rejected; Fallback(c, sample.end); return; }
        const auto rxDelta = rx - c.rx.prefix, txDelta = tx - c.tx.prefix;
        if (rxDelta || txDelta) Emit(c, rxDelta, txDelta, c.lastUtc, sample.end, Traffic::Source::EStats, Traffic::Confidence::Normal);
        m_diagnostics.estatsRx += rxDelta; m_diagnostics.estatsTx += txDelta;
        c.rx.prefix = rx; c.tx.prefix = tx; c.rx.confirmed = rx;
        for (auto* direction : { &c.rx, &c.tx })
        {
            std::vector<Span> remaining;
            for (const auto& span : direction->pending)
                if (span.second > direction->prefix) remaining.emplace_back((std::max)(span.first, direction->prefix), span.second);
            direction->pending.swap(remaining);
        }
        c.readEnd = sample.readEndCounter; c.lastUtc = sample.end;
        c.readTick = GetTickCount64();
        c.lastDataRx = sample.rx; c.lastDataTx = sample.tx;
        return;
    }
    const auto legacy = m_legacy.find(c.key);
    const auto rx = c.rx.seenPrefix, tx = c.tx.seenPrefix;
    const bool prefix = m_healthy && m_legacyHealthy && m_legacyClock == 1 && c.eligible && c.connected && !c.closed && !c.haveFin &&
        c.haveSend && c.haveReceive && sample.readBeginCounter > c.established && c.rx.seen.empty() && c.tx.seen.empty() &&
        c.rx.highest == rx && c.tx.highest == tx && legacy != m_legacy.end() && !legacy->second.unmapped &&
        legacy->second.begin >= c.begin && legacy->second.counter < sample.readBeginCounter &&
        legacy->second.rx == rx && legacy->second.tx == tx && cursor.acknowledged <= tx && cursor.received <= rx &&
        cursor.sendUnacknowledged == cursor.sendMaximum && cursor.sendNext == cursor.sendMaximum &&
        static_cast<std::uint32_t>(cursor.sendMaximum - c.sendOrigin) == static_cast<std::uint32_t>(tx) &&
        static_cast<std::uint32_t>(cursor.receiveNext - c.receiveOrigin) == static_cast<std::uint32_t>(rx);
    if (!prefix) { c.probeEnd = 0; return; }
    c.state = Traffic::ConnectionState::EStatsProbing;
    if (!c.probeEnd || sample.baseline || sample.readBeginCounter <= c.probeEnd ||
        sample.rx != c.probeRx || sample.tx != c.probeTx || rx != c.probeUniqueRx || tx != c.probeUniqueTx)
    {
        c.probeEnd = sample.readEndCounter; c.probeRx = sample.rx; c.probeTx = sample.tx;
        c.probeUniqueRx = rx; c.probeUniqueTx = tx; return;
    }
    if (m_windowCount >= 4096) { ++m_diagnostics.overflows; return; }
    c.state = Traffic::ConnectionState::EStatsActive; c.switched = true;
    c.lastUtc = sample.end; c.readEnd = sample.readEndCounter;
    c.readTick = GetTickCount64();
    c.rx.prefix = rx; c.rx.confirmed = rx; c.tx.prefix = tx;
    c.rxThroughOffset = rx - cursor.received; c.txThroughOffset = tx - cursor.acknowledged;
    c.lastDataRx = sample.rx; c.lastDataTx = sample.tx;
    m_windows[c.key].push_back({ c.begin, 0 }); ++m_windowCount; ++m_diagnostics.switches;
}
std::vector<CTcpSourceTracker::Slice> CTcpSourceTracker::TakeSlices()
{
    std::vector<Slice> result;
    result.reserve(m_slices.size());
    for (auto& entry : m_slices) result.push_back(std::move(entry.second));
    m_slices.clear();
    return result;
}
void CTcpSourceTracker::Expire()
{
    const auto now = GetTickCount64();
    for (auto it = m_connections.begin(); it != m_connections.end();)
    {
        if (it->second.state == Traffic::ConnectionState::EStatsActive && now - it->second.readTick > 2500)
            Fallback(it->second, Traffic::FileTimeNow());
        if (now - it->second.tick <= 60000) { ++it; continue; }
        Fallback(it->second, Traffic::FileTimeNow());
        const auto alias = m_aliases.find(it->second.key);
        if (alias != m_aliases.end() && alias->second == it->first) m_aliases.erase(alias);
        it = m_connections.erase(it);
    }
    for (auto it = m_legacy.begin(); it != m_legacy.end();)
        if (now - it->second.tick > 60000) it = m_legacy.erase(it); else ++it;
}
void CTcpSourceTracker::Finish()
{
    for (auto& entry : m_connections) Fallback(entry.second, Traffic::FileTimeNow());
}
CTcpSourceTracker::Diagnostics CTcpSourceTracker::GetDiagnostics() const
{
    auto result = m_diagnostics;
    result.windows = m_windowCount;
    for (const auto& entry : m_connections)
    {
        const auto state = entry.second.state;
        if (state == Traffic::ConnectionState::EStatsActive) ++result.active;
        else if (state == Traffic::ConnectionState::EStatsProbing) ++result.probing;
        else if (state == Traffic::ConnectionState::Uncertain) ++result.uncertain;
    }
    return result;
}
