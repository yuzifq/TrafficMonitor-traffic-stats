namespace
{
void NativeEndpointProbe(const wchar_t* output)
{
    std::ofstream out(output);
    WSADATA wsa{};
    Check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Endpoint probe WSAStartup failed");
    try
    {
        ProofSessionChecks(out);
        for (const bool ipv6 : { false, true })
        {
            struct Context
            {
                std::mutex mutex;
                std::vector<Traffic::NetworkEvent> events;
                std::vector<CTcpProofCollector::Event> proofEvents;
                DWORD parent{ GetCurrentProcessId() }, child{};
            } context;
            CTcpProofCollector proof;
            Check(proof.Start([](void* data, const CTcpProofCollector::Event& event) {
                auto& context = *static_cast<Context*>(data);
                std::lock_guard<std::mutex> lock(context.mutex);
                if (context.proofEvents.size() < 65536) context.proofEvents.push_back(event);
            }, &context), "Cannot start the event-ID-filtered TCP sequence observer");
            CEtwProcNetCollector collector;
            collector.m_includeLoopback = true;
            collector.SetEventSink([](void* data, const Traffic::NetworkEvent& event) {
                auto& context = *static_cast<Context*>(data);
                std::lock_guard<std::mutex> lock(context.mutex);
                if (event.key.protocol == 6 && (event.key.pid == context.parent || event.key.pid == context.child) &&
                    context.events.size() < 512) context.events.push_back(event);
            }, &context);
            collector.Start(); Sleep(1200);
            Check(collector.m_running.load(), "Endpoint ETW consumer did not start");
            ProbeChild peer(ipv6, L"steps");
            { std::lock_guard<std::mutex> lock(context.mutex); context.child = GetProcessId(peer.process); }
            SocketGuard client;
            ConnectProbe(client, ipv6, peer.port);
            sockaddr_storage local{};
            int length = sizeof(local);
            Check(getsockname(client.value, reinterpret_cast<sockaddr*>(&local), &length) == 0, "Cannot read endpoint probe local port");
            const auto port = AddressPort(local, ipv6);
            CTcpEStatsCollector estats;
            estats.m_includeLoopback = true;
            Traffic::ConnectionKey counterKey;
            bool counterFound{};
            for (const auto& sample : estats.Poll())
                if (sample.key.pid == context.parent && sample.key.localPort == port && sample.key.family == (ipv6 ? 6 : 4))
                {
                    Check(sample.enabled && !sample.error && !sample.rx && !sample.tx, "Endpoint proof baseline already contains data");
                    counterKey = sample.key; counterFound = true;
                }
            Check(counterFound, "Endpoint proof EStats identity missing");
            const auto cursorBefore = ReadProbeCounters(counterKey);
            for (const auto size : probeSteps)
            {
                TransferSend(client.value, size, 0x5a);
                TransferReceive(client.value, size, 0x6b);
            }
            Sleep(300);
            const auto cursorAfter = ReadProbeCounters(counterKey);
            estats.Stop();
            closesocket(client.value); client.value = INVALID_SOCKET;
            Sleep(2300);
            const auto proofDiagnostics = proof.GetDiagnostics();
            proof.Stop(); collector.Stop();
            std::vector<unsigned char> query(sizeof(EVENT_TRACE_PROPERTIES) + 512);
            auto* queryProperties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(query.data());
            queryProperties->Wnode.BufferSize = static_cast<ULONG>(query.size());
            queryProperties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            Check(ControlTraceW(0, proof.m_name.c_str(), queryProperties, EVENT_TRACE_CONTROL_QUERY) == ERROR_WMI_INSTANCE_NOT_FOUND,
                "Owned TCP proof trace session survived collector stop");
            unsigned mismatches{}, packets{};
            for (const auto& event : context.events)
            {
                const auto expectedLocal = event.key.pid == context.parent ? port : peer.port;
                const auto expectedRemote = event.key.pid == context.parent ? peer.port : port;
                const bool match = event.hasEndpoints && event.key.localPort == expectedLocal && event.key.remotePort == expectedRemote;
                out << (ipv6 ? 6 : 4) << '\t' << (event.key.pid == context.parent ? "client" : "server") << '\t'
                    << static_cast<unsigned>(event.opcode) << '\t' << event.bytes << '\t' << ntohs(event.key.localPort) << '\t'
                    << ntohs(event.key.remotePort) << '\t' << match << '\n';
                if (event.bytes && !event.retransmit) { ++packets; if (!match) ++mismatches; }
            }
            out << "data-summary\t" << (ipv6 ? 6 : 4) << "\tpackets\t" << packets << "\tmismatches\t" << mismatches << '\n' << std::flush;
            Check(packets && !mismatches, "ETW send/receive do not use the same local/remote endpoint identity");
            Traffic::U64 tcb{};
            Traffic::U64 begin{}, established{}, end{};
            for (const auto& event : context.proofEvents)
                if ((event.kind == CTcpProofCollector::Kind::Connect || event.kind == CTcpProofCollector::Kind::Established) &&
                    event.endpoints && event.key.pid == context.parent &&
                    event.key.localPort == port && event.key.remotePort == peer.port)
                {
                    Check(!tcb || tcb == event.tcb, "Controlled endpoint was bound to multiple TCP instances");
                    tcb = event.tcb;
                    begin = !begin ? event.timestamp : (std::min)(begin, event.timestamp);
                    if (event.kind == CTcpProofCollector::Kind::Established) established = event.timestamp;
                }
            Check(tcb != 0, "Sequence observer missed the controlled connection identity");
            for (const auto& event : context.proofEvents)
                if (event.tcb == tcb && event.timestamp > begin && event.kind == CTcpProofCollector::Kind::Close)
                    end = !end ? event.timestamp : (std::min)(end, event.timestamp);
            Check(end > begin, "Sequence observer missed the controlled connection close boundary");
            std::map<std::int64_t, std::int64_t> ranges, receiveRanges;
            Traffic::U64 sent{}; std::uint32_t origin{}; bool first = true;
            std::uint32_t receiveOrigin{}, sendIsn{}; bool haveReceiveOrigin{}, haveSendIsn{};
            for (const auto& event : context.proofEvents)
            {
                if (event.tcb != tcb || event.timestamp < begin || event.timestamp > end) continue;
                out << "proof\t" << (ipv6 ? 6 : 4) << '\t' << static_cast<unsigned>(event.kind) << '\t'
                    << event.bytes << '\t' << event.sequence << '\t' << event.state << '\n';
                if (event.kind == CTcpProofCollector::Kind::Syn) { sendIsn = event.sequence; haveSendIsn = true; }
                if (event.kind == CTcpProofCollector::Kind::Receive && event.timestamp < established && event.bytes == 1)
                {
                    receiveOrigin = event.sequence; haveReceiveOrigin = true;
                }
                if (event.kind == CTcpProofCollector::Kind::Receive && event.timestamp >= established)
                {
                    const auto left = static_cast<std::int32_t>(event.sequence - cursorBefore.RcvNxt);
                    const auto right = static_cast<std::int64_t>(left) + event.bytes;
                    auto found = receiveRanges.emplace(left, right);
                    if (!found.second) found.first->second = (std::max)(found.first->second, right);
                }
                if (event.kind != CTcpProofCollector::Kind::Send) continue;
                if (first) { origin = event.sequence; first = false; }
                const auto left = static_cast<std::int32_t>(event.sequence - origin);
                const auto right = static_cast<std::int64_t>(left) + event.bytes;
                auto found = ranges.emplace(left, right);
                if (!found.second) found.first->second = (std::max)(found.first->second, right);
                sent += event.bytes;
            }
            Traffic::U64 unique{}, expected{};
            const auto total = [](const auto& spans) {
                std::int64_t last{}; Traffic::U64 bytes{};
                for (const auto& range : spans)
                {
                    if (range.second <= last) continue;
                    bytes += range.second - (std::max)(last, range.first); last = range.second;
                }
                return bytes;
            };
            unique = total(ranges);
            for (const auto size : probeSteps) expected += size;
            out << "proof-summary\t" << (ipv6 ? 6 : 4) << "\tsent\t" << sent << "\tunique\t" << unique <<
                "\tparseFailures\t" << proofDiagnostics.parseFailures << "\tlost\t" << proofDiagnostics.eventsLost << '\n' << std::flush;
            Check(unique == expected && !proofDiagnostics.parseFailures && !proofDiagnostics.eventsLost && !proofDiagnostics.buffersLost,
                "Filtered sequence observer did not preserve known TCP byte ranges");
            out << "cursor-proof\t" << (ipv6 ? 6 : 4) << "\trxUnique\t" << total(receiveRanges) << "\ttxCursorDelta\t" <<
                static_cast<DWORD>(cursorAfter.SndMax - cursorBefore.SndMax) << "\trxCursorDelta\t" <<
                static_cast<DWORD>(cursorAfter.RcvNxt - cursorBefore.RcvNxt) << '\n' << std::flush;
            Check(established && haveSendIsn && haveReceiveOrigin && sendIsn + 1 == cursorBefore.SndMax &&
                receiveOrigin == cursorBefore.RcvNxt && origin == cursorBefore.SndMax && total(receiveRanges) == expected &&
                static_cast<DWORD>(cursorAfter.SndMax - cursorBefore.SndMax) == expected &&
                static_cast<DWORD>(cursorAfter.RcvNxt - cursorBefore.RcvNxt) == expected,
                "Native TCP lifecycle/sequence evidence did not anchor the same EStats snapshot positions");
        }
        out << "PASS\n";
    }
    catch (const std::exception& error) { out << "FAIL\t" << error.what() << '\n'; throw; }
    WSACleanup();
}
}
