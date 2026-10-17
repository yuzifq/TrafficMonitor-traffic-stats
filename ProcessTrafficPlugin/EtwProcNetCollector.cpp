#include "EtwProcNetCollector.h"

#include <evntcons.h>
#include <tdh.h>

#include <array>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")

namespace
{
constexpr UCHAR EVENT_TRACE_TYPE_SEND_VALUE = 10;
constexpr UCHAR EVENT_TRACE_TYPE_RECEIVE_VALUE = 11;
constexpr UCHAR EVENT_TRACE_TYPE_SEND_IPV6_VALUE = 26;
constexpr UCHAR EVENT_TRACE_TYPE_RECEIVE_IPV6_VALUE = 27;
constexpr ULONG PROPERTY_ARRAY_INDEX_NONE = 0xFFFFFFFF;
const GUID SYSTEM_TRACE_CONTROL_GUID = { 0x9e814aad, 0x3204, 0x11d2, { 0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39 } };
const GUID TCPIP_PROVIDER_GUID = { 0x9a280ac0, 0xc8e0, 0x11d1, { 0x84, 0xe2, 0x00, 0xc0, 0x4f, 0xb9, 0x98, 0xa2 } };
const GUID UDPIP_PROVIDER_GUID = { 0xbf3a50c5, 0xa9c9, 0x4988, { 0xa0, 0x05, 0x2d, 0xf0, 0xb7, 0xc8, 0x0f, 0x80 } };

bool IsLoopbackAddress(const unsigned char* address, ULONG size)
{
    if (size == 4)
    {
        return address[0] == 127;
    }

    bool zero_prefix = true;
    for (ULONG i = 0; i < 10; ++i)
    {
        zero_prefix = zero_prefix && address[i] == 0;
    }
    if (!zero_prefix)
    {
        return false;
    }
    if (address[10] == 0xff && address[11] == 0xff)
    {
        return address[12] == 127;
    }
    for (ULONG i = 10; i < 15; ++i)
    {
        zero_prefix = zero_prefix && address[i] == 0;
    }
    return zero_prefix && address[15] == 1;
}
}

CEtwProcNetCollector::CEtwProcNetCollector()
    : m_running(false),
      m_statusText(L"Stopped"),
      m_sessionName(KERNEL_LOGGER_NAME),
      m_sessionGuid(SYSTEM_TRACE_CONTROL_GUID),
      m_sessionHandle(0),
      m_traceHandle(INVALID_PROCESSTRACE_HANDLE),
      m_ownsSession(false)
{
}

CEtwProcNetCollector::~CEtwProcNetCollector()
{
    Stop();
}

bool CEtwProcNetCollector::Start()
{
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true))
    {
        return true;
    }
    if (m_thread.joinable()) m_thread.join();

    {
        std::lock_guard<std::mutex> lock(m_rateMutex);
        m_diagnostics = {};
        m_hasLossBaseline = false;
        LARGE_INTEGER before{}, after{}, frequency{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&before);
        FILETIME utc{};
        GetSystemTimePreciseAsFileTime(&utc);
        QueryPerformanceCounter(&after);
        m_startFileTime = (static_cast<Traffic::U64>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
        m_startCounter = before.QuadPart + (after.QuadPart - before.QuadPart) / 2;
        m_counterFrequency = frequency.QuadPart;
        m_timestampClock = 0;
    }
    UpdateStatus(L"Starting ETW session...");

    m_thread = std::thread(&CEtwProcNetCollector::WorkerLoop, this);
    return true;
}

void CEtwProcNetCollector::Stop()
{
    m_running.store(false);

    const auto trace = m_traceHandle.exchange(INVALID_PROCESSTRACE_HANDLE);
    if (trace != 0 && trace != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(trace);
    }

    if (m_thread.joinable())
    {
        m_thread.join();
    }

    StopTraceSession();
    UpdateStatus(L"Stopped");
}

std::wstring CEtwProcNetCollector::GetStatusText() const
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_statusText;
}

void CEtwProcNetCollector::SetEventSink(EventSink sink, void* context)
{
    if (m_running.load()) return;
    m_eventSink = sink;
    m_eventContext = context;
}

CEtwProcNetCollector::Diagnostics CEtwProcNetCollector::GetDiagnostics() const
{
    std::vector<unsigned char> storage(sizeof(EVENT_TRACE_PROPERTIES) + 4096);
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(storage.size());
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    const auto error = ControlTraceW(0, m_sessionName.c_str(), properties, EVENT_TRACE_CONTROL_QUERY);
    std::lock_guard<std::mutex> lock(m_rateMutex);
    m_diagnostics.queryError = error;
    if (error == ERROR_SUCCESS)
    {
        if (!m_hasLossBaseline)
        {
            m_initialEventsLost = properties->EventsLost;
            m_initialBuffersLost = properties->RealTimeBuffersLost;
            m_hasLossBaseline = true;
        }
        if (properties->EventsLost < m_initialEventsLost || properties->RealTimeBuffersLost < m_initialBuffersLost)
            ++m_diagnostics.sessionResets;
        m_diagnostics.eventsLost += properties->EventsLost >= m_initialEventsLost ? properties->EventsLost - m_initialEventsLost : properties->EventsLost;
        m_diagnostics.realtimeBuffersLost += properties->RealTimeBuffersLost >= m_initialBuffersLost ? properties->RealTimeBuffersLost - m_initialBuffersLost : properties->RealTimeBuffersLost;
        m_initialEventsLost = properties->EventsLost;
        m_initialBuffersLost = properties->RealTimeBuffersLost;
        m_diagnostics.sessionFlags = properties->EnableFlags;
        m_diagnostics.clockType = properties->Wnode.ClientContext;
    }
    return m_diagnostics;
}

bool CEtwProcNetCollector::StartTraceSession()
{
    const auto name_bytes = static_cast<ULONG>((m_sessionName.size() + 1) * sizeof(wchar_t));
    const auto buffer_size = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + name_bytes);

    m_propertiesBuffer = std::make_unique<unsigned char[]>(buffer_size);
    std::memset(m_propertiesBuffer.get(), 0, buffer_size);

    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(m_propertiesBuffer.get());
    properties->Wnode.BufferSize = buffer_size;
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1;
    properties->Wnode.Guid = m_sessionGuid;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    properties->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP;
    properties->BufferSize = 64;
    properties->FlushTimer = 1;
#ifdef EVENT_TRACE_FLAG_NETWORK_UDPIP
    properties->EnableFlags |= EVENT_TRACE_FLAG_NETWORK_UDPIP;
#endif
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    std::memcpy(m_propertiesBuffer.get() + properties->LoggerNameOffset, m_sessionName.c_str(), name_bytes);

    auto status = StartTraceW(&m_sessionHandle, m_sessionName.c_str(), properties);
    if (status == ERROR_ALREADY_EXISTS)
    {
        m_ownsSession = false;
        UpdateStatus(L"NT Kernel Logger already exists. Reusing existing session.");
    }

    else if (status != ERROR_SUCCESS)
    {
        UpdateStatus(L"StartTrace failed: " + std::to_wstring(status));
        return false;
    }
    else
    {
        m_ownsSession = true;
        UpdateStatus(L"ETW session started.");
    }

    const auto session = GetDiagnostics();
    if (session.queryError || !(session.sessionFlags & EVENT_TRACE_FLAG_NETWORK_TCPIP) ||
        (session.clockType != 1 && session.clockType != 2))
    {
        UpdateStatus(L"ETW session error: no network events or unsupported clock.");
        StopTraceSession();
        return false;
    }
    m_timestampClock = session.clockType;
    EVENT_TRACE_LOGFILEW log_file{};
    log_file.LoggerName = const_cast<LPWSTR>(m_sessionName.c_str());
    log_file.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    log_file.EventRecordCallback = &CEtwProcNetCollector::StaticEventRecordCallback;
    log_file.Context = this;

    m_traceHandle = OpenTraceW(&log_file);
    if (m_traceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        const auto error = GetLastError();
        UpdateStatus(L"OpenTrace failed: " + std::to_wstring(error));
        StopTraceSession();
        return false;
    }

    return true;
}

void CEtwProcNetCollector::StopTraceSession()
{
    const auto trace = m_traceHandle.exchange(INVALID_PROCESSTRACE_HANDLE);
    if (trace != 0 && trace != INVALID_PROCESSTRACE_HANDLE) CloseTrace(trace);

    if (m_ownsSession && m_sessionHandle != 0 && m_propertiesBuffer)
    {
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(m_propertiesBuffer.get());
        ControlTraceW(m_sessionHandle, m_sessionName.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    }

    m_sessionHandle = 0;
    m_ownsSession = false;
    m_propertiesBuffer.reset();
}

void CEtwProcNetCollector::WorkerLoop()
{
    if (!StartTraceSession())
    {
        m_running.store(false);
        return;
    }

    GetDiagnostics();
    auto trace = m_traceHandle.load();
    const auto status = m_running.load() ? ProcessTrace(&trace, 1, nullptr, nullptr) : ERROR_CANCELLED;
    if (m_running.load())
    {
        UpdateStatus(L"ProcessTrace ended: " + std::to_wstring(status));
    }

    StopTraceSession();
    m_running.store(false);
}

void CEtwProcNetCollector::HandleEventRecord(_EVENT_RECORD* event_record)
{
    if (event_record == nullptr || event_record->UserData == nullptr)
    {
        return;
    }
    // Opcodes are provider-specific; a shared kernel session can contain other event families.
    const auto& provider = event_record->EventHeader.ProviderId;
    if (!IsEqualGUID(provider, TCPIP_PROVIDER_GUID) && !IsEqualGUID(provider, UDPIP_PROVIDER_GUID))
    {
        return;
    }
    bool before_start{};
    const auto event_time = EventTime(static_cast<Traffic::U64>(event_record->EventHeader.TimeStamp.QuadPart), before_start);
    if (before_start)
    {
        std::lock_guard<std::mutex> lock(m_rateMutex);
        ++m_diagnostics.bufferedBeforeStart;
        if (!m_reportRejected) return;
    }

    const auto opcode = event_record->EventHeader.EventDescriptor.Opcode;
    const bool is_send = opcode == EVENT_TRACE_TYPE_SEND_VALUE || opcode == EVENT_TRACE_TYPE_SEND_IPV6_VALUE;
    const bool is_receive = opcode == EVENT_TRACE_TYPE_RECEIVE_VALUE || opcode == EVENT_TRACE_TYPE_RECEIVE_IPV6_VALUE;
    const bool is_tcp = IsEqualGUID(provider, TCPIP_PROVIDER_GUID);
    const bool retransmit = is_tcp && (opcode == 14 || opcode == 30);
    const bool lifecycle = is_tcp && (opcode == 12 || opcode == 13 || opcode == 15 || opcode == 16 ||
        opcode == 28 || opcode == 29 || opcode == 31 || opcode == 32);
    if (!is_send && !is_receive && !((lifecycle || retransmit) && m_eventSink))
    {
        return;
    }

    std::uint32_t process_id = 0;
    if (TryGetUInt32Property(event_record, L"PID", process_id) != ERROR_SUCCESS)
    {
        if (TryGetUInt32Property(event_record, L"ProcessId", process_id) != ERROR_SUCCESS)
        {
            std::lock_guard<std::mutex> lock(m_rateMutex);
            ++m_diagnostics.parseFailures;
            return;
        }
    }

    std::uint32_t transfer_size = 0;
    if (TryGetUInt32Property(event_record, L"size", transfer_size) != ERROR_SUCCESS)
    {
        if (TryGetUInt32Property(event_record, L"Size", transfer_size) != ERROR_SUCCESS)
        {
            std::lock_guard<std::mutex> lock(m_rateMutex);
            ++m_diagnostics.parseFailures;
            return;
        }
    }
    if (transfer_size == 0 && !lifecycle && !retransmit)
    {
        return;
    }

    // Proxy/IPC traffic on loopback does not cross a network adapter.
    const ULONG address_size = opcode >= 26 ? 16 : 4;
    std::array<unsigned char, 16> source_address{};
    std::array<unsigned char, 16> destination_address{};
    if (TryGetFixedProperty(event_record, L"saddr", source_address.data(), address_size) != ERROR_SUCCESS ||
        TryGetFixedProperty(event_record, L"daddr", destination_address.data(), address_size) != ERROR_SUCCESS)
    {
        std::lock_guard<std::mutex> lock(m_rateMutex);
        ++m_diagnostics.parseFailures;
        return;
    }
    if (IsLoopbackAddress(source_address.data(), address_size) || IsLoopbackAddress(destination_address.data(), address_size))
    {
        std::lock_guard<std::mutex> lock(m_rateMutex);
        m_diagnostics.loopbackBytes += transfer_size;
        if (!m_includeLoopback) return;
    }

    if (m_eventSink)
    {
        Traffic::NetworkEvent event;
        event.key.pid = process_id;
        event.key.family = address_size == 16 ? 6 : 4;
        event.key.protocol = is_tcp ? 6 : 17;
        event.key.local = source_address;
        event.key.remote = destination_address;
        event.timestamp = event_time;
        event.rawTimestamp = static_cast<Traffic::U64>(event_record->EventHeader.TimeStamp.QuadPart);
        event.clockType = m_timestampClock;
        event.bytes = lifecycle ? 0 : transfer_size;
        event.send = is_send || retransmit;
        event.opcode = opcode;
        event.version = event_record->EventHeader.EventDescriptor.Version;
        event.lifecycle = lifecycle;
        event.preStart = before_start;
        event.retransmit = retransmit;
        event.loopback = IsLoopbackAddress(source_address.data(), address_size) || IsLoopbackAddress(destination_address.data(), address_size);
        event.hasEndpoints = TryGetFixedProperty(event_record, L"sport", &event.key.localPort, 2) == ERROR_SUCCESS &&
            TryGetFixedProperty(event_record, L"dport", &event.key.remotePort, 2) == ERROR_SUCCESS;
        if (is_tcp)
        {
            const bool narrow = (event_record->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) != 0;
            auto status = TryGetFixedProperty(event_record, L"connid", &event.connectionId, narrow ? 4 : 8);
            if (!narrow && status == ERROR_INVALID_DATA) TryGetFixedProperty(event_record, L"connid", &event.connectionId, 4);
            if (m_captureSequence) event.hasSequence = TryGetUInt32Property(event_record, L"seqnum", event.sequence) == ERROR_SUCCESS;
        }
        {
            std::lock_guard<std::mutex> lock(m_rateMutex);
            if (!event.hasEndpoints) ++m_diagnostics.endpointFailures;
            if (event.version > 2) ++m_diagnostics.unsupportedVersions;
            if (retransmit) { ++m_diagnostics.retransmitEvents; m_diagnostics.retransmitBytes += transfer_size; }
        }
        m_eventSink(m_eventContext, event);
    }
    if (lifecycle || retransmit || before_start) return;

    std::lock_guard<std::mutex> lock(m_rateMutex);
    ++m_diagnostics.accepted;
}

Traffic::U64 CEtwProcNetCollector::EventTime(Traffic::U64 raw, bool& before_start) const
{
    if (m_timestampClock != 1 || !m_counterFrequency)
    {
        before_start = m_startFileTime && raw < m_startFileTime;
        return raw;
    }
    // Use the same monotonic clock for the startup boundary. ProcessTrace's
    // session-wide UTC conversion can lag wall time in a long-lived session.
    before_start = raw < m_startCounter;
    const auto ticks = before_start ? m_startCounter - raw : raw - m_startCounter;
    const auto elapsed = ticks / m_counterFrequency * 10000000ULL +
        ticks % m_counterFrequency * 10000000ULL / m_counterFrequency;
    return before_start ? (elapsed < m_startFileTime ? m_startFileTime - elapsed : 0) : m_startFileTime + elapsed;
}

void CEtwProcNetCollector::UpdateStatus(std::wstring status_text)
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_statusText = std::move(status_text);
}

ULONG CEtwProcNetCollector::TryGetUInt32Property(_EVENT_RECORD* event_record, const wchar_t* property_name, std::uint32_t& value) const
{
    std::uint32_t parsed = 0;
    const auto status = TryGetFixedProperty(event_record, property_name, &parsed, sizeof(parsed));
    if (status == ERROR_SUCCESS)
    {
        value = parsed;
    }
    return status;
}

ULONG CEtwProcNetCollector::TryGetFixedProperty(_EVENT_RECORD* event_record, const wchar_t* property_name, void* value, ULONG size) const
{
    PROPERTY_DATA_DESCRIPTOR descriptor{};
    descriptor.ArrayIndex = PROPERTY_ARRAY_INDEX_NONE;
    descriptor.PropertyName = reinterpret_cast<ULONGLONG>(property_name);

    ULONG property_size = 0;
    auto status = TdhGetPropertySize(event_record, 0, nullptr, 1, &descriptor, &property_size);
    if (status != ERROR_SUCCESS)
    {
        return status;
    }

    if (property_size != size)
    {
        return ERROR_INVALID_DATA;
    }

    return TdhGetProperty(event_record, 0, nullptr, 1, &descriptor, size, static_cast<PBYTE>(value));
}

void WINAPI CEtwProcNetCollector::StaticEventRecordCallback(_EVENT_RECORD* event_record)
{
    if (event_record == nullptr || event_record->UserContext == nullptr)
    {
        return;
    }

    static_cast<CEtwProcNetCollector*>(event_record->UserContext)->HandleEventRecord(event_record);
}
