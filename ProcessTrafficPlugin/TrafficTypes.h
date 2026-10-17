#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <tuple>

namespace Traffic
{
using U64 = std::uint64_t;

struct ConnectionKey
{
    std::array<unsigned char, 16> local{}, remote{};
    std::uint32_t pid{}, localScope{}, remoteScope{};
    std::uint16_t localPort{}, remotePort{}; // Network byte order, like Windows TCP/ETW rows.
    unsigned char family{}, protocol{}; // 4/6 and IP protocol number.

    bool operator<(const ConnectionKey& other) const
    {
        return std::tie(pid, protocol, family, local, remote, localPort, remotePort, localScope, remoteScope) <
            std::tie(other.pid, other.protocol, other.family, other.local, other.remote,
                other.localPort, other.remotePort, other.localScope, other.remoteScope);
    }
    bool operator==(const ConnectionKey& other) const { return !(*this < other) && !(other < *this); }
};

struct ProcessIdentity
{
    std::uint32_t pid{};
    U64 created{};
    std::wstring path, name;
};

enum class Source : unsigned char { Etw, EStats };
enum class Confidence : unsigned char { Normal, Incomplete, Unknown };
enum class ConnectionState : unsigned char { EtwOnly, EStatsProbing, EStatsActive, Uncertain };

struct NetworkEvent
{
    ConnectionKey key;
    U64 timestamp{}, connectionId{}, rawTimestamp{};
    std::uint32_t bytes{}, sequence{}, clockType{};
    unsigned char opcode{}, version{};
    bool send{}, loopback{}, hasEndpoints{}, lifecycle{}, retransmit{}, hasSequence{}, preStart{};
};

inline U64 FileTimeNow()
{
    // Defined in collectors, which include Windows headers before this file.
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return (static_cast<U64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

inline bool IsLoopback(const std::array<unsigned char, 16>& address, unsigned char family)
{
    if (family == 4) return address[0] == 127;
    for (unsigned i = 0; i < 10; ++i) if (address[i]) return false;
    if (address[10] == 255 && address[11] == 255) return address[12] == 127;
    for (unsigned i = 10; i < 15; ++i) if (address[i]) return false;
    return address[15] == 1;
}
}
