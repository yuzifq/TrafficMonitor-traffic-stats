#include <Windows.h>
#include <evntcons.h>
#include <tdh.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "TrafficSnapshotFixture.h"

#define private public
#include "../ProcessTrafficPlugin/EtwProcNetCollector.h"
#undef private

namespace
{
unsigned propertyQueries = 0;

ULONG WINAPI CountPropertyQueries(PEVENT_RECORD event, ULONG contextCount, PTDH_CONTEXT context,
    ULONG descriptorCount, PPROPERTY_DATA_DESCRIPTOR descriptor, PULONG size)
{
    ++propertyQueries;
    return TdhGetPropertySize(event, contextCount, context, descriptorCount, descriptor, size);
}
}

#define TdhGetPropertySize CountPropertyQueries
#include "../ProcessTrafficPlugin/EtwProcNetCollector.cpp"
#undef TdhGetPropertySize

// Retain decoder fixture assertions without reintroducing a second PID ledger
// into the production ETW event producer. Unified sampling is tested separately.
class DecoderFixture : public CEtwProcNetCollector
{
public:
    std::unordered_map<DWORD, ProcessTrafficSnapshot> m_snapshotByPid;
    ULONGLONG m_lastSampleTick{ GetTickCount64() };
    std::uint64_t m_sampleSequence{};
    std::unordered_map<DWORD, std::pair<std::uint64_t, std::uint64_t>> previous;
    DecoderFixture()
    {
        SetEventSink([](void* context, const Traffic::NetworkEvent& event) {
            if (event.lifecycle || event.retransmit) return;
            auto& value = static_cast<DecoderFixture*>(context)->m_snapshotByPid[event.key.pid];
            (event.send ? value.txTotalBytes : value.rxTotalBytes) += event.bytes;
        }, this);
    }
    ~DecoderFixture() { Stop(); }
    auto GetProcessTrafficSnapshot() const { return m_snapshotByPid; }
    void SampleTraffic()
    {
        const auto now = GetTickCount64();
        if (now - m_lastSampleTick < 1000) return;
        ++m_sampleSequence;
        for (auto& item : m_snapshotByPid)
        {
            auto& last = previous[item.first];
            item.second.rxSampleBytes = item.second.rxTotalBytes - last.first;
            item.second.txSampleBytes = item.second.txTotalBytes - last.second;
            item.second.sampleSequence = m_sampleSequence;
            last = { item.second.rxTotalBytes, item.second.txTotalBytes };
        }
        m_lastSampleTick = now;
    }
};
#define CEtwProcNetCollector DecoderFixture

namespace
{
const GUID tcpProvider = { 0x9a280ac0, 0xc8e0, 0x11d1, { 0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2 } };
const GUID udpProvider = { 0xbf3a50c5, 0xa9c9, 0x4988, { 0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80 } };
const GUID unrelatedProvider = { 0xcab00001, 0x1234, 0x5678, { 0x90, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67 } };
constexpr DWORD testPid = 12345;

void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct NetworkEvent
{
    EVENT_RECORD record{};
    std::array<unsigned char, 80> data{};

    NetworkEvent(const GUID& provider, UCHAR opcode, std::uint32_t bytes, bool ipv6, bool sourceLoopback = false,
        bool destinationLoopback = false, bool mapped = false)
    {
        record.EventHeader.Size = sizeof(EVENT_HEADER);
        record.EventHeader.Flags = EVENT_HEADER_FLAG_CLASSIC_HEADER | EVENT_HEADER_FLAG_64_BIT_HEADER;
        record.EventHeader.ProviderId = provider;
        record.EventHeader.ProcessId = 99999;
        record.EventHeader.EventDescriptor.Version = 2;
        record.EventHeader.EventDescriptor.Opcode = opcode;
        record.UserData = data.data();
        const bool tcpSend = IsEqualGUID(provider, tcpProvider) && (opcode == 10 || opcode == 26);
        const auto addressSize = ipv6 ? 16u : 4u;
        record.UserDataLength = static_cast<USHORT>(8 + 2 * addressSize + 4 + (tcpSend ? 8 : 0) + 4 + 8);
        std::memcpy(data.data(), &testPid, sizeof(testPid));
        std::memcpy(data.data() + 4, &bytes, sizeof(bytes));
        const auto address = [&](unsigned char* target, bool loopback, unsigned char last) {
            if (!ipv6)
            {
                target[0] = loopback ? 127 : 192;
                target[1] = loopback ? 42 : 168;
                target[2] = 0;
                target[3] = last;
            }
            else if (mapped)
            {
                target[10] = target[11] = 0xff;
                target[12] = loopback ? 127 : 192;
                target[13] = loopback ? 42 : 168;
                target[15] = last;
            }
            else if (loopback)
            {
                target[15] = 1;
            }
            else
            {
                target[0] = 0x20;
                target[1] = 0x01;
                target[2] = 0x0d;
                target[3] = 0xb8;
                target[15] = last;
            }
        };
        address(data.data() + 8, destinationLoopback, 2);
        address(data.data() + 8 + addressSize, sourceLoopback, 1);
    }
};

void CheckExactBytes()
{
    for (const auto& provider : { tcpProvider, udpProvider })
    {
        for (const bool ipv6 : { false, true })
        {
            CEtwProcNetCollector collector;
            NetworkEvent send(provider, ipv6 ? 26 : 10, 1200, ipv6);
            NetworkEvent receive(provider, ipv6 ? 27 : 11, 777, ipv6);
            collector.HandleEventRecord(&send.record);
            collector.HandleEventRecord(&receive.record);
            Check(collector.m_snapshotByPid.size() == 1, "Network event PID was not read from its payload");
            auto& traffic = collector.m_snapshotByPid.at(testPid);
            Check(traffic.txTotalBytes == 1200 && traffic.rxTotalBytes == 777, "Network byte count or direction changed");
            for (int i = 0; i < 100; ++i) collector.HandleEventRecord(&send.record);
            Check(traffic.txTotalBytes == 121200, "Repeated packets were multiplied or incorrectly deduplicated");
            collector.m_lastSampleTick = GetTickCount64() - 1000;
            collector.SampleTraffic();
            const auto first = collector.GetProcessTrafficSnapshot().at(testPid);
            Check(first.txSampleBytes == 121200 && first.rxSampleBytes == 777,
                "The first sample discarded traffic received during startup");
            collector.HandleEventRecord(&send.record);
            collector.m_lastSampleTick = GetTickCount64() - 1000;
            collector.SampleTraffic();
            const auto sample = collector.GetProcessTrafficSnapshot().at(testPid);
            Check(sample.txSampleBytes == 1200 && sample.rxSampleBytes == 0, "Sampling re-added cumulative totals");
            const auto again = collector.GetProcessTrafficSnapshot().at(testPid);
            Check(again.txTotalBytes == sample.txTotalBytes && again.sampleSequence == sample.sampleSequence,
                "Reading a snapshot counted the same sample again");
            collector.m_lastSampleTick = GetTickCount64() - 1000;
            collector.SampleTraffic();
            const auto idle = collector.GetProcessTrafficSnapshot().at(testPid);
            Check(idle.txSampleBytes == 0 && idle.rxSampleBytes == 0, "Idle sampling repeated the last increment");
        }
    }
    std::cout << "PASS: real TDH schemas for TCP/UDP IPv4/IPv6, exact bytes, payload PID, sample deltas" << std::endl;
}

void CheckLoopback()
{
    for (const auto& provider : { tcpProvider, udpProvider })
    {
        for (const bool ipv6 : { false, true })
        {
            for (const bool send : { false, true })
            {
                for (const bool source : { false, true })
                {
                    CEtwProcNetCollector collector;
                    NetworkEvent event(provider, static_cast<UCHAR>((ipv6 ? 26 : 10) + (send ? 0 : 1)),
                        1000000, ipv6, source, !source);
                    collector.HandleEventRecord(&event.record);
                    Check(collector.m_snapshotByPid.empty(), "Loopback traffic was counted as network traffic");
                    if (ipv6)
                    {
                        NetworkEvent mapped(provider, event.record.EventHeader.EventDescriptor.Opcode,
                            1000000, true, source, !source, true);
                        collector.HandleEventRecord(&mapped.record);
                        Check(collector.m_snapshotByPid.empty(), "IPv4-mapped IPv6 loopback was counted");
                    }
                }
            }
        }
    }
    std::cout << "PASS: loopback excluded in both directions (127/8, ::1, ::ffff:127/8)" << std::endl;
}

void CheckProxyForwarding()
{
    CEtwProcNetCollector collector;
    constexpr std::uint32_t responseBytes = 1024 * 1024;
    NetworkEvent internetResponse(tcpProvider, 11, responseBytes, false);
    NetworkEvent localForward(tcpProvider, 10, responseBytes, false, true, true);
    collector.HandleEventRecord(&internetResponse.record);
    collector.HandleEventRecord(&localForward.record);
    const auto& traffic = collector.m_snapshotByPid.at(testPid);
    Check(traffic.rxTotalBytes == responseBytes && traffic.txTotalBytes == 0,
        "Forwarding a downloaded response to a local proxy client inflated uploads");

    NetworkEvent mappedExternal(udpProvider, 26, 600, true, false, false, true);
    collector.HandleEventRecord(&mappedExternal.record);
    Check(traffic.txTotalBytes == 600, "Non-loopback mapped IPv6 traffic was dropped");
    for (const auto& provider : { tcpProvider, udpProvider })
    {
        NetworkEvent legacy(provider, 10, 700, false);
        legacy.record.EventHeader.EventDescriptor.Version = 1;
        collector.HandleEventRecord(&legacy.record);
    }
    Check(traffic.txTotalBytes == 2000, "Version 1 IPv4 events stopped contributing traffic");
    std::cout << "PASS: proxy download is not an upload, non-loopback mapped IPv6 and version 1 preserved" << std::endl;
}

void CheckDisplayReadsPreserveSamples()
{
    CEtwProcNetCollector collector;
    collector.SampleTraffic();
    NetworkEvent receive(tcpProvider, 11, 4096, false);
    NetworkEvent send(udpProvider, 10, 1024, false);
    collector.HandleEventRecord(&receive.record);
    collector.HandleEventRecord(&send.record);
    const auto sequenceBeforeDisplay = collector.m_sampleSequence;
    collector.m_lastSampleTick = GetTickCount64() - 1000;
    const auto display = collector.GetProcessTrafficSnapshot().at(testPid);
    Check(collector.m_sampleSequence == sequenceBeforeDisplay,
        "A display read advanced sampling before history could consume it");
    Check(display.rxTotalBytes == 4096 && display.txTotalBytes == 1024, "Display totals lost incoming traffic");
    collector.HandleEventRecord(&receive.record);
    collector.GetProcessTrafficSnapshot();
    collector.SampleTraffic();
    const auto history = collector.GetProcessTrafficSnapshot().at(testPid);
    Check(history.rxSampleBytes == 8192 && history.txSampleBytes == 1024,
        "Traffic observed by the display was missing from the history sample");
    const auto sequenceBeforeIdleDisplay = collector.m_sampleSequence;
    collector.m_lastSampleTick = GetTickCount64() - 1000;
    collector.GetProcessTrafficSnapshot();
    Check(collector.m_sampleSequence == sequenceBeforeIdleDisplay,
        "An idle display read erased the history sample");
    collector.SampleTraffic();
    const auto idle = collector.GetProcessTrafficSnapshot().at(testPid);
    Check(idle.rxSampleBytes == 0 && idle.txSampleBytes == 0, "The next history sample repeated old traffic");
    std::cout << "PASS: display reads preserve pending traffic, delayed history sample, no repeated increments" << std::endl;
}

void CheckIgnoredEvents()
{
    CEtwProcNetCollector collector;
    collector.HandleEventRecord(nullptr);
    const auto queriesBeforeUnrelated = propertyQueries;
    for (const auto opcode : { 10, 11, 26, 27 })
    {
        NetworkEvent unrelated(unrelatedProvider, static_cast<UCHAR>(opcode), 0xffffffff, opcode >= 26);
        collector.HandleEventRecord(&unrelated.record);
    }
    Check(propertyQueries == queriesBeforeUnrelated, "Unrelated providers reached the network property decoder");
    for (const auto& provider : { tcpProvider, udpProvider })
    {
        for (const auto opcode : { 12, 13, 14, 15, 17, 18, 28, 29, 30, 31, 34 })
        {
            NetworkEvent control(provider, static_cast<UCHAR>(opcode), 0xffffffff, opcode >= 26);
            collector.HandleEventRecord(&control.record);
        }
        for (const auto length : { 4, 8, 15 })
        {
            NetworkEvent truncated(provider, 10, 0xffffffff, false);
            truncated.record.UserDataLength = static_cast<USHORT>(length);
            collector.HandleEventRecord(&truncated.record);
        }
        NetworkEvent empty(provider, 10, 0, false);
        collector.HandleEventRecord(&empty.record);
    }
    Check(collector.m_snapshotByPid.empty(), "Non-network, control, malformed or empty events entered the counters");
    NetworkEvent event(tcpProvider, 10, 1200, false);
    std::uint32_t value = 0x12345678;
    Check(collector.TryGetUInt32Property(&event.record, L"sport", value) == ERROR_INVALID_DATA && value == 0x12345678,
        "A 16-bit field was accepted as a 32-bit traffic counter");
    std::cout << "PASS: unrelated providers, connection/retransmit events, malformed and empty events ignored" << std::endl;
}

void CheckConnectionMetadata()
{
    std::vector<Traffic::NetworkEvent> events;
    CEtwProcNetCollector collector;
    collector.SetEventSink([](void* context, const Traffic::NetworkEvent& event) {
        static_cast<std::vector<Traffic::NetworkEvent>*>(context)->push_back(event);
    }, &events);
    for (const auto& provider : { tcpProvider, udpProvider })
    {
        for (const bool ipv6 : { false, true })
        {
            NetworkEvent packet(provider, ipv6 ? 26 : 10, 65537, ipv6);
            const auto portsAt = ipv6 ? 40 : 16;
            const std::uint16_t dport = 0x3412, sport = 0x7856;
            std::memcpy(packet.data.data() + portsAt, &dport, 2);
            std::memcpy(packet.data.data() + portsAt + 2, &sport, 2);
            packet.record.EventHeader.TimeStamp.QuadPart = 134000000000000000ULL;
            collector.HandleEventRecord(&packet.record);
            const auto& event = events.back();
            Check(event.hasEndpoints && event.key.localPort == sport && event.key.remotePort == dport,
                "ETW ports were guessed, swapped or read with the wrong size");
            Check(event.key.pid == testPid && event.key.family == (ipv6 ? 6 : 4) &&
                event.key.protocol == (IsEqualGUID(provider, tcpProvider) ? 6 : 17) && event.send &&
                event.bytes == 65537 && event.timestamp == 134000000000000000ULL && event.rawTimestamp == 134000000000000000ULL,
                "ETW connection envelope lost PID, family, protocol, direction, bytes or event timestamp");
        }
    }
    NetworkEvent connect(tcpProvider, 12, 0, false);
    connect.record.UserDataLength = static_cast<USHORT>(connect.data.size());
    collector.HandleEventRecord(&connect.record);
    Check(events.size() == 5 && events.back().lifecycle && events.back().bytes == 0,
        "Lifecycle events contributed bytes or did not reach the connection tracker");
    std::cout << "PASS: connection metadata, network-order ports, event timestamps, TCP/UDP parity and lifecycle envelope" << std::endl;
}
}

int main()
{
    try
    {
        CheckExactBytes();
        CheckLoopback();
        CheckProxyForwarding();
        CheckDisplayReadsPreserveSamples();
        CheckIgnoredEvents();
        CheckConnectionMetadata();
        CEtwProcNetCollector collector;
        collector.Start();
        Check(collector.m_thread.joinable(), "Start returned without launching an ETW worker");
        collector.Stop();
        collector.Start();
        Check(collector.m_thread.joinable(), "ETW failed to restart after a stopped or failed worker");
        collector.Stop();
        std::cout << "PASS: ETW worker launch, stop and restart" << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
