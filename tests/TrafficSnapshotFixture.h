#pragma once
#include <cstdint>

struct ProcessTrafficSnapshot
{
    std::uint64_t rxTotalBytes{}, txTotalBytes{}, rxSampleBytes{}, txSampleBytes{}, sampleSequence{};
};
