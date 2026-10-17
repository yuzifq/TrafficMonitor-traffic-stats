#pragma once

#include <Windows.h>
#include <evntrace.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "TrafficTypes.h"

struct _EVENT_RECORD;

class CEtwProcNetCollector
{
public:
    using EventSink = void (*)(void*, const Traffic::NetworkEvent&);
    struct Diagnostics
    {
        std::uint64_t accepted{}, parseFailures{}, endpointFailures{}, loopbackBytes{}, unsupportedVersions{}, bufferedBeforeStart{},
            eventsLost{}, realtimeBuffersLost{}, sessionResets{}, retransmitEvents{}, retransmitBytes{};
        ULONG sessionFlags{}, queryError{}, clockType{};
    };

    CEtwProcNetCollector();
    ~CEtwProcNetCollector();

    bool Start();
    void Stop();

    std::wstring GetStatusText() const;
    void SetEventSink(EventSink sink, void* context);
    Diagnostics GetDiagnostics() const;

private:
    bool StartTraceSession();
    void StopTraceSession();
    void WorkerLoop();
    void HandleEventRecord(_EVENT_RECORD* event_record);
    Traffic::U64 EventTime(Traffic::U64 raw, bool& before_start) const;
    void UpdateStatus(std::wstring status_text);
    ULONG TryGetFixedProperty(_EVENT_RECORD* event_record, const wchar_t* property_name, void* value, ULONG size) const;
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
    std::atomic<TRACEHANDLE> m_traceHandle;
    bool m_ownsSession;
    bool m_includeLoopback{ false };
    bool m_captureSequence{ false };
    bool m_reportRejected{ false };
    EventSink m_eventSink{};
    void* m_eventContext{};
    mutable Diagnostics m_diagnostics;
    mutable ULONG m_initialEventsLost{}, m_initialBuffersLost{};
    mutable bool m_hasLossBaseline{};
    Traffic::U64 m_startFileTime{};
    Traffic::U64 m_startCounter{}, m_counterFrequency{};
    ULONG m_timestampClock{};
    std::unique_ptr<unsigned char[]> m_propertiesBuffer;

};
