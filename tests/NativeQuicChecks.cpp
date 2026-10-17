#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace
{
struct WinHttpHandle
{
    HINTERNET value{};
    ~WinHttpHandle() { if (value) WinHttpCloseHandle(value); }
};
void NativeQuicProbe(const wchar_t* output)
{
    std::ofstream out(output);
    Check(static_cast<bool>(out), "Cannot write native QUIC report");
    const auto require = [&](BOOL success, const char* operation) {
        if (success) return;
        const auto error = GetLastError();
        out << "native_error\t" << operation << '\t' << error << '\n' << std::flush;
        throw std::runtime_error(operation);
    };
    CHybridTrafficCollector hybrid;
    hybrid.m_etw.SetEventSink([](void* context, const Traffic::NetworkEvent& event) {
        if (event.key.pid == GetCurrentProcessId() && event.key.protocol == 17 &&
            (event.key.localPort == htons(443) || event.key.remotePort == htons(443)))
            static_cast<CHybridTrafficCollector*>(context)->OnEvent(event);
    }, &hybrid);
    try
    {
        WinHttpHandle session{ WinHttpOpen(L"ProcessTrafficPlugin-native-test/1.2.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
        require(session.value != nullptr, "WinHttpOpen");
        require(WinHttpSetTimeouts(session.value, 5000, 5000, 15000, 15000), "WinHttpSetTimeouts");
        WinHttpHandle connection{ WinHttpConnect(session.value, L"speed.cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, 0) };
        require(connection.value != nullptr, "WinHttpConnect");
        WinHttpHandle request{ WinHttpOpenRequest(connection.value, L"GET", L"/__down?bytes=1048576",
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) };
        require(request.value != nullptr, "WinHttpOpenRequest");
        DWORD enabled = WINHTTP_PROTOCOL_FLAG_HTTP3;
        BOOL required = TRUE;
        require(WinHttpSetOption(request.value, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &enabled, sizeof(enabled)), "Enable HTTP3");
        require(WinHttpSetOption(request.value, WINHTTP_OPTION_HTTP_PROTOCOL_REQUIRED, &required, sizeof(required)), "Require HTTP3 (no TCP fallback)");
        Check(hybrid.Start(), "Cannot start QUIC accounting collector");
        const auto ready = GetTickCount64() + 10000;
        while (hybrid.m_etw.m_traceHandle == INVALID_PROCESSTRACE_HANDLE && GetTickCount64() < ready) Sleep(50);
        Check(hybrid.m_etw.m_traceHandle != INVALID_PROCESSTRACE_HANDLE, "QUIC ETW consumer did not become ready");
        require(WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0), "HTTP3 request");
        require(WinHttpReceiveResponse(request.value, nullptr), "HTTP3 response");
        DWORD protocol{}, size = sizeof(protocol), status{}, statusSize = sizeof(status);
        require(WinHttpQueryOption(request.value, WINHTTP_OPTION_HTTP_PROTOCOL_USED, &protocol, &size), "Query actual protocol");
        require(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX), "Query HTTP status");
        out << "protocol_used\t" << protocol << "\tstatus\t" << status << '\n' << std::flush;
        Check(protocol == WINHTTP_PROTOCOL_FLAG_HTTP3 && status == 200, "Native request did not actually use HTTP3");
        std::array<char, 65536> bytes;
        Traffic::U64 body{};
        for (;;)
        {
            DWORD count{};
            require(WinHttpReadData(request.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count), "Read HTTP3 body");
            if (!count) break;
            body += count;
        }
        Check(body == 1048576, "HTTP3 endpoint did not provide the requested application body length");
        Sleep(5000); hybrid.Stop();
        Traffic::U64 rx{}, tx{};
        for (const auto& delta : hybrid.PeekPending().deltas) { rx += delta.rx; tx += delta.tx; }
        const auto d = hybrid.GetDiagnostics();
        out << "body\t" << body << "\traw_udp\t" << d.rawRx << '\t' << d.rawTx << "\tformal_udp\t" << rx << '\t' << tx
            << "\testats\t" << d.sources.estatsRx << '\t' << d.sources.estatsTx << '\n';
        Check(d.rawRx >= body && d.rawTx && rx == d.rawRx && tx == d.rawTx &&
            !d.sources.estatsRx && !d.sources.estatsTx && !d.etw.eventsLost && !d.etw.parseFailures,
            "Actual QUIC datagrams were lost, duplicated or assigned to TCP EStats");
        out << "PASS: OS-confirmed HTTP3, unchanged UDP ledger, application bytes not substituted for encrypted datagrams\n";
    }
    catch (const std::exception& error) { out << "NOT_PASSED\t" << error.what() << '\n'; throw; }
}
}
