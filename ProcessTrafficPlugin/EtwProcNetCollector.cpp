#include "EtwProcNetCollector.h"

#include <evntcons.h>
#include <tdh.h>

#include <array>
#include <cstring>
#include <cwchar>
#include <utility>

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
}

CEtwProcNetCollector::CEtwProcNetCollector()
    : m_running(false),
      m_statusText(L"Stopped"),
      m_sessionName(KERNEL_LOGGER_NAME),
      m_sessionGuid(SYSTEM_TRACE_CONTROL_GUID),
      m_sessionHandle(0),
      m_traceHandle(INVALID_PROCESSTRACE_HANDLE),
      m_ownsSession(false),
      m_lastSampleTick(0)
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

    {
        std::lock_guard<std::mutex> lock(m_rateMutex);
        m_snapshotByPid.clear();
        m_lastRxTotalByPid.clear();
        m_lastTxTotalByPid.clear();
        m_lastSampleTick = 0;
    }
    UpdateStatus(L"Starting ETW session...");

    m_thread = std::thread(&CEtwProcNetCollector::WorkerLoop, this);
    return true;
}

void CEtwProcNetCollector::Stop()
{
    if (!m_running.exchange(false))
    {
        return;
    }

    if (m_traceHandle != 0 && m_traceHandle != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(m_traceHandle);
    }

    if (m_thread.joinable())
    {
        m_thread.join();
    }

    StopTraceSession();
    UpdateStatus(L"Stopped");
}

std::unordered_map<DWORD, CEtwProcNetCollector::ProcessTrafficSnapshot> CEtwProcNetCollector::GetProcessTrafficSnapshot() const
{
    RefreshRates();
    std::lock_guard<std::mutex> lock(m_rateMutex);
    return m_snapshotByPid;
}

std::wstring CEtwProcNetCollector::GetStatusText() const
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_statusText;
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
        wchar_t buffer[96]{};
        swprintf_s(buffer, L"StartTrace failed: %lu", status);
        UpdateStatus(buffer);
        return false;
    }
    else
    {
        m_ownsSession = true;
        UpdateStatus(L"ETW session started.");
    }

    EVENT_TRACE_LOGFILEW log_file{};
    log_file.LoggerName = const_cast<LPWSTR>(m_sessionName.c_str());
    log_file.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log_file.EventRecordCallback = &CEtwProcNetCollector::StaticEventRecordCallback;
    log_file.Context = this;

    m_traceHandle = OpenTraceW(&log_file);
    if (m_traceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        const auto error = GetLastError();
        wchar_t buffer[96]{};
        swprintf_s(buffer, L"OpenTrace failed: %lu", error);
        UpdateStatus(buffer);
        StopTraceSession();
        return false;
    }

    return true;
}

void CEtwProcNetCollector::StopTraceSession()
{
    if (m_traceHandle != 0 && m_traceHandle != INVALID_PROCESSTRACE_HANDLE)
    {
        m_traceHandle = INVALID_PROCESSTRACE_HANDLE;
    }

    if (m_ownsSession && m_sessionHandle != 0 && m_propertiesBuffer)
    {
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(m_propertiesBuffer.get());
        ControlTraceW(m_sessionHandle, m_sessionName.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    }

    m_sessionHandle = 0;
    m_ownsSession = false;
    m_propertiesBuffer.reset();
}

void CEtwProcNetCollector::RefreshRates() const
{
    std::lock_guard<std::mutex> lock(m_rateMutex);

    const auto now = GetTickCount64();
    if (m_lastSampleTick == 0)
    {
        m_lastSampleTick = now;
        for (const auto& entry : m_snapshotByPid)
        {
            m_lastRxTotalByPid[entry.first] = entry.second.rxTotalBytes;
            m_lastTxTotalByPid[entry.first] = entry.second.txTotalBytes;
        }
        return;
    }

    const auto elapsed = now - m_lastSampleTick;
    if (elapsed < 1000)
    {
        return;
    }

    for (auto& entry : m_snapshotByPid)
    {
        const auto last_rx = m_lastRxTotalByPid[entry.first];
        const auto last_tx = m_lastTxTotalByPid[entry.first];
        entry.second.rxBytesPerSec = ((entry.second.rxTotalBytes - last_rx) * 1000ULL) / elapsed;
        entry.second.txBytesPerSec = ((entry.second.txTotalBytes - last_tx) * 1000ULL) / elapsed;
        m_lastRxTotalByPid[entry.first] = entry.second.rxTotalBytes;
        m_lastTxTotalByPid[entry.first] = entry.second.txTotalBytes;
    }

    m_lastSampleTick = now;
}

void CEtwProcNetCollector::WorkerLoop()
{
    if (!StartTraceSession())
    {
        m_running.store(false);
        return;
    }

    const auto status = ProcessTrace(&m_traceHandle, 1, nullptr, nullptr);
    if (m_running.load())
    {
        wchar_t buffer[96]{};
        swprintf_s(buffer, L"ProcessTrace ended: %lu", status);
        UpdateStatus(buffer);
    }

    StopTraceSession();
}

void CEtwProcNetCollector::HandleEventRecord(_EVENT_RECORD* event_record)
{
    const auto opcode = event_record->EventHeader.EventDescriptor.Opcode;
    const bool is_send = opcode == EVENT_TRACE_TYPE_SEND_VALUE || opcode == EVENT_TRACE_TYPE_SEND_IPV6_VALUE;
    const bool is_receive = opcode == EVENT_TRACE_TYPE_RECEIVE_VALUE || opcode == EVENT_TRACE_TYPE_RECEIVE_IPV6_VALUE;
    if (!is_send && !is_receive)
    {
        return;
    }

    std::uint32_t process_id = 0;
    if (TryGetUInt32Property(event_record, L"PID", process_id) != ERROR_SUCCESS)
    {
        if (TryGetUInt32Property(event_record, L"ProcessId", process_id) != ERROR_SUCCESS)
        {
            return;
        }
    }

    std::uint32_t transfer_size = 0;
    if (TryGetUInt32Property(event_record, L"size", transfer_size) != ERROR_SUCCESS)
    {
        if (TryGetUInt32Property(event_record, L"Size", transfer_size) != ERROR_SUCCESS)
        {
            return;
        }
    }

    std::lock_guard<std::mutex> lock(m_rateMutex);
    auto& snapshot = m_snapshotByPid[process_id];
    if (is_send)
    {
        snapshot.txTotalBytes += transfer_size;
    }
    else
    {
        snapshot.rxTotalBytes += transfer_size;
    }
}

void CEtwProcNetCollector::UpdateStatus(std::wstring status_text)
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_statusText = std::move(status_text);
}

ULONG CEtwProcNetCollector::TryGetUInt32Property(_EVENT_RECORD* event_record, const wchar_t* property_name, std::uint32_t& value) const
{
    PROPERTY_DATA_DESCRIPTOR descriptor{};
    descriptor.ArrayIndex = PROPERTY_ARRAY_INDEX_NONE;
    descriptor.PropertyName = reinterpret_cast<ULONGLONG>(property_name);

    ULONG property_size = 0;
    auto status = TdhGetPropertySize(event_record, 0, nullptr, 1, &descriptor, &property_size);
    if (status != ERROR_SUCCESS || property_size == 0)
    {
        return status;
    }

    std::array<unsigned char, sizeof(std::uint32_t)> buffer{};
    if (property_size > buffer.size())
    {
        return ERROR_INSUFFICIENT_BUFFER;
    }

    status = TdhGetProperty(event_record, 0, nullptr, 1, &descriptor, property_size, buffer.data());
    if (status != ERROR_SUCCESS)
    {
        return status;
    }

    std::memcpy(&value, buffer.data(), property_size);
    return ERROR_SUCCESS;
}

void WINAPI CEtwProcNetCollector::StaticEventRecordCallback(_EVENT_RECORD* event_record)
{
    if (event_record == nullptr || event_record->UserContext == nullptr)
    {
        return;
    }

    static_cast<CEtwProcNetCollector*>(event_record->UserContext)->HandleEventRecord(event_record);
}
