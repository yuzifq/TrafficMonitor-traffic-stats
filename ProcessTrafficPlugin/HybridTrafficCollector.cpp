#include "HybridTrafficCollector.h"

#include <algorithm>
#include <chrono>

using Traffic::U64;
namespace
{
void Merge(CHybridTrafficCollector::Delta& target, const CHybridTrafficCollector::Delta& delta)
{
    if (target.identity.empty()) target = delta;
    else
    {
        target.rx += delta.rx;
        target.tx += delta.tx;
        target.start = (std::min)(target.start, delta.start);
        target.end = (std::max)(target.end, delta.end);
        target.confidence = (std::max)(target.confidence, delta.confidence);
    }
}
}

CHybridTrafficCollector::CHybridTrafficCollector()
{
    m_etw.SetEventSink(EventCallback, this);
}
CHybridTrafficCollector::~CHybridTrafficCollector() { Stop(); }

bool CHybridTrafficCollector::SetCommitSink(CommitSink sink, void* context)
{
    std::lock_guard<std::mutex> control(m_controlMutex);
    if (m_running) return false;
    m_commitSink = sink;
    m_commitContext = context;
    return true;
}

bool CHybridTrafficCollector::Start()
{
    std::lock_guard<std::mutex> control(m_controlMutex);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) return true;
        m_running = true;
        m_lastPublish = Traffic::FileTimeNow();
        m_lastPublishTick = GetTickCount64();
        m_diagnostics.running = true;
        m_sources.Reset();
    }
    m_etw.Start();
    m_proof.Start(ProofCallback, this);
    try { m_thread = std::thread(&CHybridTrafficCollector::Run, this); }
    catch (...)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_running = false;
            m_diagnostics.running = false;
        }
        m_etw.Stop();
        m_proof.Stop();
        return false;
    }
    return true;
}

void CHybridTrafficCollector::Stop()
{
    std::lock_guard<std::mutex> control(m_controlMutex);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running && !m_thread.joinable()) return;
        m_running = false;
    }
    m_wake.notify_all();
    m_etw.Stop();
    if (m_thread.joinable()) m_thread.join();
    const auto& lastSamples = m_estats.Poll();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& sample : lastSamples) m_sources.Observe(sample);
    }
    m_proof.Stop();
    m_estats.Stop();
    std::map<Traffic::ConnectionKey, Raw> remaining;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        remaining.swap(m_raw);
    }
    const auto now = Traffic::FileTimeNow();
    ProcessRaw(remaining, now);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sources.Finish();
    DrainSources();
    Publish(now);
    m_diagnostics.running = false;
}

void CHybridTrafficCollector::EventCallback(void* context, const Traffic::NetworkEvent& event)
{
    static_cast<CHybridTrafficCollector*>(context)->OnEvent(event);
}

void CHybridTrafficCollector::ProofCallback(void* context, const CTcpProofCollector::Event& event)
{
    auto& collector = *static_cast<CHybridTrafficCollector*>(context);
    if (!collector.m_includeProofLoopback && event.endpoints &&
        (Traffic::IsLoopback(event.key.local, event.key.family) || Traffic::IsLoopback(event.key.remote, event.key.family))) return;
    std::lock_guard<std::mutex> lock(collector.m_mutex);
    collector.m_sources.OnProof(event);
}

void CHybridTrafficCollector::OnEvent(const Traffic::NetworkEvent& event)
{
    if ((!event.bytes && !event.lifecycle) || event.retransmit || event.preStart) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sources.KeepLegacy(event)) return;
    auto key = event.key;
    if (m_raw.size() >= 16384 && m_raw.find(key) == m_raw.end())
    {
        ++m_diagnostics.queueOverflows;
        key = {}; // Retain bytes explicitly as unknown instead of dropping them.
    }
    auto& raw = m_raw[key];
    raw.first = !raw.first ? event.timestamp : (std::min)(raw.first, event.timestamp);
    if (raw.last.connectionId && event.connectionId && raw.last.connectionId != event.connectionId)
        raw.mixedInstances = true;
    raw.last = event;
    if (event.lifecycle)
    {
        raw.opened |= event.opcode == 12 || event.opcode == 15 || event.opcode == 28 || event.opcode == 31;
        raw.closed |= event.opcode == 13 || event.opcode == 29;
    }
    raw.end = (std::max)(raw.end, event.timestamp);
    (event.send ? raw.tx : raw.rx) += event.bytes;
}

std::wstring CHybridTrafficCollector::Identity(const Traffic::ProcessIdentity& owner, U64 generation)
{
    if (!owner.path.empty() && owner.created)
    {
        std::wstring identity = L"path:" + owner.path;
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, identity.data(), static_cast<int>(identity.size()),
            identity.data(), static_cast<int>(identity.size()), nullptr, nullptr, 0);
        return identity;
    }
    return L"unknown:" + std::to_wstring(owner.pid) + L":" + std::to_wstring(owner.created) + L":" + std::to_wstring(generation);
}

void CHybridTrafficCollector::ProcessRaw(std::map<Traffic::ConnectionKey, Raw>& raw, U64 now)
{
    static_cast<void>(now);
    const auto tick = GetTickCount64();
    std::map<DWORD, Traffic::ProcessIdentity> owners;
    for (const auto& entry : raw)
        if (entry.first.pid && owners.find(entry.first.pid) == owners.end())
            owners.emplace(entry.first.pid, CTcpEStatsCollector::QueryOwner(entry.first.pid));
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& entry : raw)
    {
        const auto& key = entry.first;
        const auto& value = entry.second;
        Flow untracked;
        const auto existing = m_flows.find(key);
        const bool overflow = existing == m_flows.end() && m_flows.size() >= 16384;
        auto& flow = overflow ? untracked : m_flows[key];
        if (overflow) ++m_diagnostics.queueOverflows;
        if (!flow.first || value.opened || (flow.connectionId && value.last.connectionId && flow.connectionId != value.last.connectionId))
        {
            flow = {};
            flow.first = value.first;
            flow.generation = ++m_generation;
        }
        const auto owner = owners.find(key.pid);
        if (owner != owners.end() && owner->second.created && owner->second.created <= value.first && !value.mixedInstances)
        {
            if (flow.owner.created && flow.owner.created != owner->second.created)
            {
                flow = {};
                flow.first = value.first;
                flow.generation = ++m_generation;
            }
            flow.owner = owner->second;
        }
        else if (owner != owners.end() && owner->second.created > value.first)
        {
            // The event predates the currently owning process: do not relabel it
            // with a reused PID. Retain its amount under an unknown identity.
            flow.owner = {};
        }
        if (value.mixedInstances)
        {
            flow.owner = {};
            flow.generation = ++m_generation;
        }
        flow.owner.pid = key.pid;
        flow.lastTick = tick;
        flow.connectionId = value.last.connectionId;
        if (!value.rx && !value.tx)
        {
            if (value.closed && !overflow) m_flows.erase(key);
            continue;
        }
        Delta delta;
        delta.identity = Identity(flow.owner, flow.generation);
        delta.name = flow.owner.name.empty() ? L"PID" + std::to_wstring(key.pid) : flow.owner.name;
        delta.path = flow.owner.path;
        delta.rx = value.rx; delta.tx = value.tx;
        delta.start = value.first; delta.end = value.end;
        if (m_diagnostics.etw.eventsLost || m_diagnostics.etw.realtimeBuffersLost || m_diagnostics.etw.sessionResets)
            delta.confidence = Traffic::Confidence::Incomplete;
        if (!flow.owner.created || flow.owner.path.empty() || value.mixedInstances || !key.pid)
        {
            delta.confidence = Traffic::Confidence::Unknown;
            m_diagnostics.unknownRx += delta.rx;
            m_diagnostics.unknownTx += delta.tx;
        }
        Accumulate(delta);
        if (value.closed && !overflow) m_flows.erase(key);
    }
    for (auto it = m_flows.begin(); it != m_flows.end();)
    {
        if (tick - it->second.lastTick > 60000) it = m_flows.erase(it);
        else ++it;
    }
}

void CHybridTrafficCollector::Accumulate(const Delta& delta)
{
    const LedgerKey key{ delta.identity, delta.source };
    auto& current = m_current[delta.identity];
    Merge(static_cast<Delta&>(current), delta);
    current.sources |= 1u << static_cast<unsigned>(delta.source);
    Merge(m_pending[key], delta);
}

void CHybridTrafficCollector::DrainSources()
{
    for (const auto& slice : m_sources.TakeSlices())
    {
        Delta delta;
        delta.identity = Identity(slice.owner, slice.generation); delta.name = slice.owner.name;
        delta.path = slice.owner.path; delta.rx = slice.rx; delta.tx = slice.tx;
        delta.start = slice.start; delta.end = slice.end; delta.source = slice.source; delta.confidence = slice.confidence;
        Accumulate(delta);
    }
    m_diagnostics.sources = m_sources.GetDiagnostics();
}

void CHybridTrafficCollector::Publish(U64 now)
{
    const auto tick = GetTickCount64();
    const auto elapsed = tick > m_lastPublishTick ? (tick - m_lastPublishTick) * 10000ULL : 10000000ULL;
    m_snapshot.clear();
    for (auto& entry : m_current)
    {
        auto& snapshot = entry.second;
        snapshot.rxPerSec = snapshot.rx * 10000000ULL / elapsed;
        snapshot.txPerSec = snapshot.tx * 10000000ULL / elapsed;
        m_snapshot.push_back(std::move(snapshot));
    }
    m_current.clear();
    m_lastPublish = now;
    m_lastPublishTick = tick;
    ++m_diagnostics.publications;
    m_diagnostics.trackedConnections = static_cast<std::uint32_t>(m_flows.size());
    m_diagnostics.probing = m_diagnostics.sources.probing;
    m_diagnostics.active = m_diagnostics.sources.active;
    m_diagnostics.uncertain = m_diagnostics.sources.uncertain;
}

void CHybridTrafficCollector::Run()
{
    U64 cycle{};
    for (;;)
    {
        std::map<Traffic::ConnectionKey, Raw> raw;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_wake.wait_for(lock, std::chrono::milliseconds(500), [&] { return !m_running; })) break;
            raw.swap(m_raw);
        }
        const auto now = Traffic::FileTimeNow();
        ProcessRaw(raw, now);
        const bool sample = (++cycle % 2) == 0;
        const auto& observations = m_estats.Poll(sample);
        const auto etw = sample ? m_etw.GetDiagnostics() : CEtwProcNetCollector::Diagnostics{};
        const auto proof = m_proof.GetDiagnostics();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (sample)
            {
                const bool proofHealthy = proof.running && !proof.error && !proof.parseFailures && !proof.eventsLost && !proof.buffersLost;
                const bool legacyHealthy = !etw.queryError && !etw.parseFailures && !etw.eventsLost && !etw.realtimeBuffersLost && !etw.sessionResets;
                m_sources.SetHealth(proofHealthy, etw.clockType, legacyHealthy);
            }
            for (const auto& observation : observations) m_sources.Observe(observation);
            m_sources.Expire();
            DrainSources();
            if (sample)
            {
                m_diagnostics.etw = etw;
                m_diagnostics.estats = m_estats.GetDiagnostics();
                m_diagnostics.proof = proof;
                Publish(now);
            }
        }
        if (sample && m_commitSink)
        {
            // The sink drains immutable batches and checkpoints even if the
            // host stops requesting UI data. Never call it under m_mutex.
            try { m_commitSink(m_commitContext); }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_diagnostics.commitFailures;
            }
        }
    }
}

std::vector<CHybridTrafficCollector::AppSnapshot> CHybridTrafficCollector::GetSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

CHybridTrafficCollector::Batch CHybridTrafficCollector::PeekPending()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_inflight.sequence && !m_pending.empty())
    {
        m_inflight.sequence = ++m_sequence;
        for (auto& entry : m_pending) m_inflight.deltas.push_back(std::move(entry.second));
        m_pending.clear();
    }
    return m_inflight;
}

void CHybridTrafficCollector::Acknowledge(U64 sequence)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (sequence && sequence == m_inflight.sequence) m_inflight = {};
}

CHybridTrafficCollector::Diagnostics CHybridTrafficCollector::GetDiagnostics() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_diagnostics;
}

std::wstring CHybridTrafficCollector::GetStatusText() const { return m_etw.GetStatusText(); }
