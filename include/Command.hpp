#pragma once
#include "Types.hpp"

namespace engine::core
{

    enum class CommandType : std::uint16_t
    {
        AddOrder = 0,
        CancelOrder = 1,
        ModifyOrder = 2,
    };

    struct alignas(64) Command
    {
        CommandType type;
        types::OrderId orderId;
        types::InstrumentId instrumentId;
        types::ClientId clientId;
        types::TimeInForce tif;
        types::OrderType orderType;
        types::Verb verb;
        types::Price limitPrice;
        types::Price stopPrice;
        types::Quantity qty;
    };

    static_assert(sizeof(Command) <= 64, "Command struct must fit in one cache line (64 bytes)");

}
