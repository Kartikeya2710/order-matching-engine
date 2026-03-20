#pragma once

namespace engine::types
{
    using OrderId = std::uint64_t;
    using ClientId = std::uint64_t;
    using Quantity = std::uint64_t;
    using Price = std::uint64_t;
    using InstrumentId = std::uint64_t;

    enum class Verb : std::uint16_t
    {
        Buy = 0,
        Sell = 1,
    };

    enum class OrderType : std::uint16_t
    {
        Market = 0,
        Limit = 1,
        Stop = 2
    };

    enum class TimeInForce : std::uint16_t
    {
        FOK = 0,
        IOC = 1,
        GTC = 2,
        None = 3,
    };
}
