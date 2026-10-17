namespace
{
void TcpProofParserChecks()
{
    CTcpProofCollector collector;
    collector.m_frequency = 10000000;
    collector.m_startQpc = 100; collector.m_startUtc = 134000000000000000ULL;
    std::vector<CTcpProofCollector::Event> received;
    collector.m_sink = [](void* context, const CTcpProofCollector::Event& event) {
        static_cast<std::vector<CTcpProofCollector::Event>*>(context)->push_back(event);
    };
    collector.m_context = &received;
    EVENT_RECORD record{};
    record.EventHeader.ProviderId = kTcpProofProvider;
    record.EventHeader.TimeStamp.QuadPart = 200;
    std::array<unsigned char, 64> data{};
    record.UserData = data.data(); record.UserContext = &collector;
    for (const bool narrow : { false, true })
    {
        const auto pointer = narrow ? 4u : 8u;
        const Traffic::U64 tcb = narrow ? 0x87654321ULL : 0xFEDCBA9887654321ULL;
        record.EventHeader.Flags = narrow ? EVENT_HEADER_FLAG_32_BIT_HEADER : EVENT_HEADER_FLAG_64_BIT_HEADER;
        for (const USHORT id : { static_cast<USHORT>(1332), static_cast<USHORT>(1333), static_cast<USHORT>(1074) })
        {
            const bool send = id != 1074;
            data.fill(0);
            const std::uint32_t bytes = 65537, sequence = 0; // Zero is a valid wrapped TCP sequence number.
            std::memcpy(data.data(), &tcb, pointer);
            std::memcpy(data.data() + pointer + (send ? 8 : 0), &bytes, 4);
            std::memcpy(data.data() + pointer + (send ? 12 : 4), &sequence, 4);
            record.EventHeader.EventDescriptor.Id = id;
            record.EventHeader.EventDescriptor.Version = 0;
            record.UserDataLength = static_cast<USHORT>(pointer + (send ? 16 : 8));
            const auto count = received.size();
            collector.Handle(&record);
            Check(received.size() == count + 1 && received.back().tcb == tcb && received.back().bytes == bytes &&
                received.back().sequence == 0 && received.back().timestamp == collector.m_startUtc + 100 && received.back().counter == 200 &&
                received.back().kind == (send ? CTcpProofCollector::Kind::Send : CTcpProofCollector::Kind::Receive),
                "Real modern TCP metadata lost pointer width, byte size, sequence zero, direction or clock mapping");
            --record.UserDataLength;
            collector.Handle(&record);
            Check(received.size() == count + 1, "Truncated modern TCP event reached the evidence sink");
        }
    }
    const auto accepted = received.size();
    record.EventHeader.TimeStamp.QuadPart = 99;
    collector.Handle(&record);
    Check(received.size() == accepted && collector.m_diagnostics.beforeStart == 1,
        "Pre-start modern TCP observation reached the evidence sink");
    record.EventHeader.TimeStamp.QuadPart = 200; ++record.UserDataLength;
    collector.m_sink = [](void*, const CTcpProofCollector::Event&) { throw std::runtime_error("injected sink failure"); };
    CTcpProofCollector::Callback(&record);
    Check(collector.m_diagnostics.error == ERROR_INVALID_DATA, "Evidence callback exception escaped the Windows ETW callback");
    std::cout << "PASS: modern TCP TDH-validated prefixes, 32/64-bit pointers, sequence wrap zero, truncation, clocks and callback failures\n";
}
}
