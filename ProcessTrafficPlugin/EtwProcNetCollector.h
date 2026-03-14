#pragma once

#include <Windows.h>
#include <evntrace.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

struct _EVENT_RECORD;

class CEtwProcNetCollector
{
public:
    struct ProcessTrafficSnapshot
    {
        std::uint64_t rxBytesPerSec{};
        std::uint64_t txBytesPerSec{};
        std::uint64_t rxTotalBytes{};
        std::uint64_t txTotalBytes{};
    };

    CEtwProcNetCollector();
    ~CEtwProcNetCollector();

    bool Start();
    void Stop();

    std::unordered_map<DWORD, ProcessTrafficSnapshot> GetProcessTrafficSnapshot() const;
    std::wstring GetStatusText() const;

private:
    bool StartTraceSession();
    void StopTraceSession();
    void RefreshRates() const;
    void WorkerLoop();
    void HandleEventRecord(_EVENT_RECORD* event_record);
    void UpdateStatus(std::wstring status_text);
    ULONG TryGetUInt32Property(_EVENT_RECORD* event_record, const wchar_t* property_name, std::uint32_t& value) const;

    static void WINAPI StaticEventRecordCallback(_EVENT_RECORD* event_record);

private:
    std::atomic<bool> m_running;
    std::thread m_thread;

    mutable std::mutex m_statusMutex;
    mutable std::mutex m_rateMutex;
    std::wstring m_statusText;
    std::wstring m_sessionName;
    GUID m_sessionGuid;
    TRACEHANDLE m_sessionHandle;
    TRACEHANDLE m_traceHandle;
    bool m_ownsSession;
    mutable ULONGLONG m_lastSampleTick;
    std::unique_ptr<unsigned char[]> m_propertiesBuffer;

    mutable std::unordered_map<DWORD, ProcessTrafficSnapshot> m_snapshotByPid;
    mutable std::unordered_map<DWORD, std::uint64_t> m_lastRxTotalByPid;
    mutable std::unordered_map<DWORD, std::uint64_t> m_lastTxTotalByPid;
};
