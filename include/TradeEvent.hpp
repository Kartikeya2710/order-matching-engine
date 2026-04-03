#pragma once
#include "Types.hpp"
#include <cstdint>

namespace engine::core
{

    struct TradeEvent
    {
        enum class Type : uint8_t
        {
            OrderAccepted = 0,
            OrderRejected = 1,
            OrderCancelled = 2,
            Fill = 3,
            PartialFill = 4,
        };

        Type type;
        types::InstrumentId instrumentId = 0;
        types::OrderId aggressorOrderId = 0;
        types::OrderId passiveOrderId = 0;
        types::ClientId aggressorClientId = 0;
        types::ClientId passiveClientId = 0;
        types::Price fillPrice = 0;
        types::Quantity fillQty = 0;
        types::Quantity aggressorRemaining = 0;
        types::Quantity passiveRemaining = 0;
    };

}