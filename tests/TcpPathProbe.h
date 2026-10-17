#pragma once

namespace
{
// Test-only attribution of retransmissions. The production collector still
// enables Data exclusively; Path is never a DLL runtime accounting source.
class NativePathProbe
{
    Traffic::ConnectionKey key;
    bool owned{};
    DWORD Configure(bool enabled)
    {
        TCP_ESTATS_PATH_RW_v0 rw{ static_cast<BOOLEAN>(enabled) };
        if (key.family == 6)
        {
            auto row = Row6(key);
            return SetPerTcp6ConnectionEStats(&row, TcpConnectionEstatsPath, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);
        }
        auto row = Row4(key);
        return SetPerTcpConnectionEStats(&row, TcpConnectionEstatsPath, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);
    }
    DWORD Query(TCP_ESTATS_PATH_RW_v0& rw, TCP_ESTATS_PATH_ROD_v0& data)
    {
        if (key.family == 6)
        {
            auto row = Row6(key);
            return GetPerTcp6ConnectionEStats(&row, TcpConnectionEstatsPath, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw),
                nullptr, 0, 0, reinterpret_cast<PUCHAR>(&data), 0, sizeof(data));
        }
        auto row = Row4(key);
        return GetPerTcpConnectionEStats(&row, TcpConnectionEstatsPath, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw),
            nullptr, 0, 0, reinterpret_cast<PUCHAR>(&data), 0, sizeof(data));
    }
public:
    explicit NativePathProbe(const Traffic::ConnectionKey& value) : key(value)
    {
        TCP_ESTATS_PATH_RW_v0 rw{};
        TCP_ESTATS_PATH_ROD_v0 data{};
        Check(Query(rw, data) == NO_ERROR, "Cannot query test-only Path statistics");
        if (!rw.EnableCollection)
        {
            Check(Configure(true) == NO_ERROR, "Cannot enable test-only Path statistics");
            owned = true;
        }
    }
    ~NativePathProbe() { Stop(); }
    void Stop() { if (owned) { Configure(false); owned = false; } }
    TCP_ESTATS_PATH_ROD_v0 Read()
    {
        TCP_ESTATS_PATH_RW_v0 rw{};
        TCP_ESTATS_PATH_ROD_v0 data{};
        Check(Query(rw, data) == NO_ERROR && rw.EnableCollection, "Cannot read enabled Path statistics");
        return data;
    }
};
}
