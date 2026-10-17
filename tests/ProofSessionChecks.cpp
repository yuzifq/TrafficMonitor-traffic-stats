namespace
{
struct ProofSessionFixture
{
    TRACEHANDLE handle{};
    GUID guid{};
    std::wstring name;
    std::vector<BYTE> storage;
    explicit ProofSessionFixture(bool stale)
    {
        Check(SUCCEEDED(CoCreateGuid(&guid)), "Cannot create cleanup fixture GUID");
        wchar_t text[40]{};
        StringFromGUID2(guid, text, 40);
        FILETIME created{}, exited{}, kernel{}, user{};
        Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE, "Cannot read fixture birth");
        const auto birth = (static_cast<Traffic::U64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        name = L"PTP-TcpProof-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(stale ? 1 : birth) + L"-" + text;
        storage.resize(sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t));
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
        properties->Wnode.BufferSize = static_cast<ULONG>(storage.size());
        properties->Wnode.Guid = guid; properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        properties->Wnode.ClientContext = 1;
        properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        Check(StartTraceW(&handle, name.c_str(), properties) == ERROR_SUCCESS, "Cannot start cleanup fixture");
    }
    ULONG Query() { return ControlTraceW(handle, name.c_str(), reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data()), EVENT_TRACE_CONTROL_QUERY); }
    ~ProofSessionFixture() { ControlTraceW(handle, name.c_str(), reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data()), EVENT_TRACE_CONTROL_STOP); }
};
void ProofSessionChecks(std::ostream& out)
{
    ProofSessionFixture abandoned(true), live(false);
    RemoveAbandonedProofSessions();
    Check(abandoned.Query() == ERROR_WMI_INSTANCE_NOT_FOUND && live.Query() == ERROR_SUCCESS,
        "Session recovery stopped a live owner or retained a stale/reused-PID owner");
    out << "PASS: abandoned proof-session cleanup validates GUID, PID and process birth; live owners preserved\n";
}
}
