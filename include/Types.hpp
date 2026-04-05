#pragma once
#include <cstdint>

namespace engine::types
{
    using OrderId = uint32_t;
    using ClientId = uint32_t;
    using Quantity = uint32_t;
    using Price = uint32_t;
    using InstrumentId = uint32_t;

    enum class Verb : uint16_t
    {
        Buy = 0,
        Sell = 1,
    };

    enum class OrderType : uint16_t
    {
        Market = 0,
        Limit = 1,
        Stop = 2
    };

    enum class TimeInForce : uint16_t
    {
        FOK = 0,
        IOC = 1,
        GTC = 2,
        None = 3,
    };
}
