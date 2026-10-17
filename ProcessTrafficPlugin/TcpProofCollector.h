#pragma once

#include <Windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "TrafficTypes.h"

// Optional sequence evidence, never an additive traffic source. Legacy ETW
// remains available if this provider/filter/schema cannot be enabled.
class CTcpProofCollector
{
public:
    enum class Kind : unsigned char { Connect, Established, Close, State, Syn, Fin, Send, Receive };
    struct Event
    {
        Traffic::ConnectionKey key;
        Traffic::U64 tcb{}, timestamp{}, counter{};
        // Receive lengths can include SYN/FIN sequence-space observations.
        // They require lifecycle validation before being called payload bytes.
        std::uint32_t bytes{}, sequence{}, state{}, oldState{};
        Kind kind{};
        bool endpoints{};
    };
    struct Diagnostics
    {
        Traffic::U64 accepted{}, parseFailures{}, beforeStart{}, controls{}, eventsLost{}, buffersLost{};
        ULONG error{};
        bool running{};
    };
    using Sink = void (*)(void*, const Event&);
    ~CTcpProofCollector();
    bool Start(Sink sink, void* context);
    void Stop();
    Diagnostics GetDiagnostics();

private:
    static void WINAPI Callback(EVENT_RECORD* record);
    void Handle(EVENT_RECORD* record);
    void Run();
    void StopLocked();
    bool ValidatePrefix(EVENT_RECORD* record, bool send);
    std::mutex m_control, m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_running{};
    std::atomic<TRACEHANDLE> m_trace{ INVALID_PROCESSTRACE_HANDLE };
    TRACEHANDLE m_session{};
    std::wstring m_name;
    std::vector<unsigned char> m_properties;
    std::array<std::array<unsigned char, 256>, 3> m_schemas{};
    Traffic::U64 m_startQpc{}, m_startUtc{}, m_frequency{};
    Sink m_sink{};
    void* m_context{};
    Diagnostics m_diagnostics;
};
