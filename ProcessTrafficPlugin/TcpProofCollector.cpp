#include <winsock2.h>
#include <ws2tcpip.h>
#include <evntcons.h>
#include <tdh.h>
#include <objbase.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <string_view>
#include "TcpProofCollector.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
const GUID kTcpProofProvider = { 0x2f07e2ee, 0x15db, 0x40f1, { 0x90, 0xef, 0x9d, 0x7b, 0xa2, 0x82, 0x18, 0x8a } };
void RemoveAbandonedProofSessions()
{
    constexpr unsigned count = 128, bytes = sizeof(EVENT_TRACE_PROPERTIES) + 2048;
    std::vector<BYTE> storage(count * bytes);
    EVENT_TRACE_PROPERTIES* properties[count]{};
    for (unsigned i = 0; i < count; ++i)
    {
        auto* item = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data() + i * bytes);
        item->Wnode.BufferSize = bytes;
        item->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        properties[i] = item;
    }
    ULONG returned{};
    if (QueryAllTracesW(properties, count, &returned) != ERROR_SUCCESS || returned > count) return;
    constexpr wchar_t prefix[] = L"PTP-TcpProof-";
    for (ULONG i = 0; i < returned; ++i)
    {
        auto* item = properties[i];
        if (item->LoggerNameOffset < sizeof(EVENT_TRACE_PROPERTIES) || item->LoggerNameOffset >= bytes) continue;
        const auto* name = reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(item) + item->LoggerNameOffset);
        const auto capacity = (bytes - item->LoggerNameOffset) / sizeof(wchar_t);
        if (wcsnlen_s(name, capacity) == capacity || wcsncmp(name, prefix, std::size(prefix) - 1)) continue;
        wchar_t* next{};
        const auto pid = wcstoul(name + std::size(prefix) - 1, &next, 10);
        if (!pid || *next++ != L'-') continue;
        const auto birth = _wcstoui64(next, &next, 10);
        if (!birth || *next++ != L'-') continue;
        GUID guid{};
        if (FAILED(CLSIDFromString(next, &guid)) || !IsEqualGUID(guid, item->Wnode.Guid)) continue;
        bool abandoned{};
        const auto process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) abandoned = GetLastError() == ERROR_INVALID_PARAMETER;
        else
        {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(process, &created, &exited, &kernel, &user))
                abandoned = ((static_cast<Traffic::U64>(created.dwHighDateTime) << 32) | created.dwLowDateTime) != birth;
            abandoned |= WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
            CloseHandle(process);
        }
        if (abandoned) ControlTraceW(0, name, item, EVENT_TRACE_CONTROL_STOP);
    }
}
ULONG ProofProperty(EVENT_RECORD* record, const wchar_t* name, void* target, ULONG capacity)
{
    PROPERTY_DATA_DESCRIPTOR descriptor{};
    descriptor.PropertyName = reinterpret_cast<ULONGLONG>(name);
    descriptor.ArrayIndex = ULONG_MAX;
    ULONG size{};
    if (TdhGetPropertySize(record, 0, nullptr, 1, &descriptor, &size) != ERROR_SUCCESS || !size || size > capacity) return 0;
    return TdhGetProperty(record, 0, nullptr, 1, &descriptor, size, static_cast<BYTE*>(target)) == ERROR_SUCCESS ? size : 0;
}
bool ProofEndpoints(EVENT_RECORD* record, CTcpProofCollector::Event& event)
{
    sockaddr_storage local{}, remote{};
    const auto localSize = ProofProperty(record, L"LocalAddress", &local, sizeof(local));
    const auto remoteSize = ProofProperty(record, L"RemoteAddress", &remote, sizeof(remote));
    if (!localSize || !remoteSize || local.ss_family != remote.ss_family) return false;
    auto& key = event.key;
    key.protocol = IPPROTO_TCP;
    if (local.ss_family == AF_INET && localSize == sizeof(sockaddr_in) && remoteSize == sizeof(sockaddr_in))
    {
        const auto& a = reinterpret_cast<const sockaddr_in&>(local);
        const auto& b = reinterpret_cast<const sockaddr_in&>(remote);
        key.family = 4; key.localPort = a.sin_port; key.remotePort = b.sin_port;
        std::memcpy(key.local.data(), &a.sin_addr, 4); std::memcpy(key.remote.data(), &b.sin_addr, 4);
    }
    else if (local.ss_family == AF_INET6 && localSize == sizeof(sockaddr_in6) && remoteSize == sizeof(sockaddr_in6))
    {
        const auto& a = reinterpret_cast<const sockaddr_in6&>(local);
        const auto& b = reinterpret_cast<const sockaddr_in6&>(remote);
        key.family = 6; key.localPort = a.sin6_port; key.remotePort = b.sin6_port;
        key.localScope = a.sin6_scope_id; key.remoteScope = b.sin6_scope_id;
        std::memcpy(key.local.data(), &a.sin6_addr, 16); std::memcpy(key.remote.data(), &b.sin6_addr, 16);
    }
    else return false;
    return true;
}
}

CTcpProofCollector::~CTcpProofCollector() { Stop(); }

bool CTcpProofCollector::Start(Sink sink, void* context)
{
    std::lock_guard<std::mutex> control(m_control);
    if (m_running) return false;
    StopLocked();
    if (m_session) return false;
    m_sink = sink; m_context = context;
    for (auto& schema : m_schemas) schema.fill(0);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_diagnostics = {};
    }
    LARGE_INTEGER before{}, after{}, frequency{};
    FILETIME utc{};
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&before);
    GetSystemTimePreciseAsFileTime(&utc); QueryPerformanceCounter(&after);
    m_frequency = frequency.QuadPart;
    m_startQpc = before.QuadPart + (after.QuadPart - before.QuadPart) / 2;
    m_startUtc = static_cast<Traffic::U64>(utc.dwHighDateTime) << 32 | utc.dwLowDateTime;
    GUID sessionGuid{};
    const auto guidResult = CoCreateGuid(&sessionGuid);
    if (FAILED(guidResult) || !m_frequency)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_diagnostics.error = ERROR_NOT_SUPPORTED;
        return false;
    }
    wchar_t guidText[40]{};
    StringFromGUID2(sessionGuid, guidText, static_cast<int>(std::size(guidText)));
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto birth = (static_cast<Traffic::U64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    RemoveAbandonedProofSessions();
    m_name = L"PTP-TcpProof-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(birth) + L"-" + guidText;
    m_properties.assign(sizeof(EVENT_TRACE_PROPERTIES) + (m_name.size() + 1) * sizeof(wchar_t), 0);
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(m_properties.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(m_properties.size());
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.Guid = sessionGuid;
    properties->Wnode.ClientContext = 1;
    properties->BufferSize = 64; properties->MinimumBuffers = 8; properties->MaximumBuffers = 64;
    properties->FlushTimer = 1; properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    std::memcpy(m_properties.data() + properties->LoggerNameOffset, m_name.c_str(), (m_name.size() + 1) * sizeof(wchar_t));
    TRACEHANDLE session{};
    auto error = StartTraceW(&session, m_name.c_str(), properties);
    if (error == ERROR_SUCCESS)
    {
        m_session = session;
        struct Filter { BOOLEAN filterIn; UCHAR reserved; USHORT count; USHORT ids[10]; } filter{
            TRUE, 0, 10, { 1002, 1004, 1017, 1033, 1043, 1051, 1074, 1176, 1332, 1333 } };
        EVENT_FILTER_DESCRIPTOR descriptor{};
        descriptor.Ptr = reinterpret_cast<ULONGLONG>(&filter);
        descriptor.Size = sizeof(filter); descriptor.Type = EVENT_FILTER_TYPE_EVENT_ID;
        ENABLE_TRACE_PARAMETERS parameters{};
        parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
        parameters.EnableFilterDesc = &descriptor; parameters.FilterDescCount = 1;
        // Do not enable Summary: it can alter global TCP statistics collection.
        error = EnableTraceEx2(m_session, &kTcpProofProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
            TRACE_LEVEL_VERBOSE, 0x1700000484ULL, 0, 0, &parameters);
    }
    if (error == ERROR_SUCCESS)
    {
        EVENT_TRACE_LOGFILEW file{};
        file.LoggerName = m_name.data(); file.Context = this;
        file.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        file.EventRecordCallback = Callback;
        m_trace = OpenTraceW(&file);
        if (m_trace == INVALID_PROCESSTRACE_HANDLE) error = GetLastError();
    }
    if (error == ERROR_SUCCESS)
    {
        m_running = true;
        try { m_thread = std::thread(&CTcpProofCollector::Run, this); }
        catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
    }
    if (error != ERROR_SUCCESS)
    {
        StopLocked();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_diagnostics.error = error;
        return false;
    }
    return true;
}

void CTcpProofCollector::StopLocked()
{
    m_running = false;
    const auto trace = m_trace.load();
    bool stopped = !m_session;
    if (m_session)
    {
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(m_properties.data());
        const auto error = ControlTraceW(m_session, m_name.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (error == ERROR_SUCCESS)
        {
            m_diagnostics.eventsLost = properties->EventsLost;
            m_diagnostics.buffersLost = properties->RealTimeBuffersLost;
        }
        if (error == ERROR_SUCCESS || error == ERROR_WMI_INSTANCE_NOT_FOUND) m_session = 0;
        else m_diagnostics.error = error;
        stopped = error == ERROR_SUCCESS || error == ERROR_WMI_INSTANCE_NOT_FOUND;
    }
    if (!stopped && trace != INVALID_PROCESSTRACE_HANDLE) CloseTrace(trace);
    if (m_thread.joinable()) m_thread.join();
    if (stopped && trace != INVALID_PROCESSTRACE_HANDLE) CloseTrace(trace);
    m_trace = INVALID_PROCESSTRACE_HANDLE;
}
void CTcpProofCollector::Stop()
{
    std::lock_guard<std::mutex> control(m_control);
    StopLocked();
}
void CTcpProofCollector::Run()
{
    auto handle = m_trace.load();
    const auto error = ProcessTrace(&handle, 1, nullptr, nullptr);
    if (m_running.exchange(false))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_diagnostics.error = error;
    }
}
CTcpProofCollector::Diagnostics CTcpProofCollector::GetDiagnostics()
{
    std::lock_guard<std::mutex> control(m_control);
    std::vector<unsigned char> storage(sizeof(EVENT_TRACE_PROPERTIES) + (m_name.size() + 1) * sizeof(wchar_t));
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(storage.size());
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    const auto error = m_session ? ControlTraceW(m_session, m_name.c_str(), properties, EVENT_TRACE_CONTROL_QUERY) : ERROR_NOT_READY;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (error == ERROR_SUCCESS) { m_diagnostics.eventsLost = properties->EventsLost; m_diagnostics.buffersLost = properties->RealTimeBuffersLost; }
    else if (m_session) m_diagnostics.error = error;
    auto result = m_diagnostics;
    result.running = m_running;
    return result;
}
void WINAPI CTcpProofCollector::Callback(EVENT_RECORD* record)
{
    if (!record || !record->UserContext) return;
    auto& collector = *static_cast<CTcpProofCollector*>(record->UserContext);
    try { collector.Handle(record); }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(collector.m_mutex);
        ++collector.m_diagnostics.parseFailures;
        collector.m_diagnostics.error = ERROR_INVALID_DATA;
    }
}
bool CTcpProofCollector::ValidatePrefix(EVENT_RECORD* record, bool send)
{
    const auto id = record->EventHeader.EventDescriptor.Id;
    auto& state = m_schemas[send ? (id == 1333 ? 1 : 0) : 2][record->EventHeader.EventDescriptor.Version];
    if (state) return state == 1;
    state = 2;
    ULONG size{};
    if (TdhGetEventInformation(record, 0, nullptr, nullptr, &size) != ERROR_INSUFFICIENT_BUFFER) return false;
    std::vector<unsigned char> storage(size);
    auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(storage.data());
    if (TdhGetEventInformation(record, 0, nullptr, info, &size) != ERROR_SUCCESS) return false;
    const wchar_t* sendNames[]{ L"Tcb", L"Cwnd", L"SndWnd", L"BytesSent", L"SeqNo" };
    const wchar_t* receiveNames[]{ L"Tcb", L"NumBytes", L"SeqNo" };
    const auto count = send ? std::size(sendNames) : std::size(receiveNames);
    if (info->TopLevelPropertyCount < count || size < offsetof(TRACE_EVENT_INFO, EventPropertyInfoArray) +
        count * sizeof(EVENT_PROPERTY_INFO)) return false;
    for (size_t i = 0; i < count; ++i)
    {
        const auto& property = info->EventPropertyInfoArray[i];
        const auto* expected = send ? sendNames[i] : receiveNames[i];
        if (property.Flags || property.count != 1 || property.NameOffset >= size ||
            property.nonStructType.InType != (i ? TDH_INTYPE_UINT32 : TDH_INTYPE_POINTER)) return false;
        const auto* name = reinterpret_cast<const wchar_t*>(storage.data() + property.NameOffset);
        const auto capacity = (size - property.NameOffset) / sizeof(wchar_t);
        const auto length = wcsnlen_s(name, capacity);
        if (length == capacity || std::wstring_view(name, length) != expected) return false;
    }
    state = 1;
    return true;
}
void CTcpProofCollector::Handle(EVENT_RECORD* record)
{
    if (!record->UserData || !IsEqualGUID(record->EventHeader.ProviderId, kTcpProofProvider)) return;
    const auto id = record->EventHeader.EventDescriptor.Id;
    Event event;
    const auto raw = static_cast<Traffic::U64>(record->EventHeader.TimeStamp.QuadPart);
    event.counter = raw;
    if (raw < m_startQpc)
    {
        std::lock_guard<std::mutex> lock(m_mutex); ++m_diagnostics.beforeStart;
        return;
    }
    const auto elapsed = raw - m_startQpc;
    event.timestamp = m_startUtc + elapsed / m_frequency * 10000000ULL + elapsed % m_frequency * 10000000ULL / m_frequency;
    bool valid{};
    if (id == 1332 || id == 1333 || id == 1074)
    {
        const bool send = id != 1074;
        const auto pointer = record->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER ? 4u : 8u;
        if (ValidatePrefix(record, send) && record->UserDataLength >= pointer + (send ? 16 : 8))
        {
            const auto* payload = static_cast<const unsigned char*>(record->UserData);
            std::memcpy(&event.tcb, payload, pointer);
            std::memcpy(&event.bytes, payload + pointer + (send ? 8 : 0), 4);
            std::memcpy(&event.sequence, payload + pointer + (send ? 12 : 4), 4);
            event.kind = send ? Kind::Send : Kind::Receive;
            valid = true;
            if (!event.bytes) { std::lock_guard<std::mutex> lock(m_mutex); ++m_diagnostics.controls; return; }
        }
    }
    else
    {
        valid = ProofProperty(record, L"Tcb", &event.tcb, sizeof(event.tcb)) != 0;
        if (id == 1002 || id == 1017 || id == 1033)
        {
            event.kind = id == 1002 ? Kind::Connect : Kind::Established;
            event.endpoints = ProofEndpoints(record, event);
            if (id == 1002) event.key.pid = record->EventHeader.ProcessId;
            else
            {
                DWORD status{};
                valid &= ProofProperty(record, L"ProcessId", &event.key.pid, 4) == 4 &&
                    ProofProperty(record, L"Status", &status, 4) == 4;
                if (status) return;
            }
            valid &= event.endpoints;
        }
        else if (id == 1043) event.kind = Kind::Close;
        else if (id == 1051)
        {
            event.kind = Kind::State;
            valid &= ProofProperty(record, L"OldState", &event.oldState, 4) == 4 && ProofProperty(record, L"NewState", &event.state, 4) == 4;
        }
        else if (id == 1004) { event.kind = Kind::Syn; valid &= ProofProperty(record, L"ISN", &event.sequence, 4) == 4; }
        else if (id == 1176) { event.kind = Kind::Fin; valid &= ProofProperty(record, L"RcvNxt", &event.sequence, 4) == 4; }
        else return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!valid || !event.tcb) { ++m_diagnostics.parseFailures; return; }
        ++m_diagnostics.accepted;
    }
    if (m_sink) m_sink(m_context, event);
}
