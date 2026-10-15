#pragma once
#include <type_traits>
#include <variant>

#include "Types.hpp"

namespace engine::core
{

    struct NewOrder
    {
        types::OrderId orderId;
        types::ClientId clientId;
        types::Verb side;
        types::OrderType orderType;
        types::TimeInForce tif;
        types::Price limitPrice;
        types::Quantity qty;
    };

    struct CancelOrder
    {
        types::OrderId orderId;
        types::ClientId clientId;
    };

    struct ModifyOrder
    {
        types::OrderId orderId;
        types::ClientId clientId;
        types::Price newLimitPrice;
        types::Quantity newQty;
    };

    using Request = std::variant<NewOrder, CancelOrder, ModifyOrder>;

    struct alignas(64) Command
    {
        types::InstrumentId instrumentId;
        Request request;
    };

    static_assert(sizeof(Command) <= 64, "Command struct must fit in one cache line (64 bytes)");
    static_assert(
        std::is_trivially_copyable_v<Command>,
        "Command must be trivially copyable so the ring buffer can copy it into a slot"
    );

    constexpr bool hasValidEnums(const Command& cmd) noexcept
    {
        const auto* order = std::get_if<NewOrder>(&cmd.request);
        if (order == nullptr)
        {
            return true;
        }

        return order->side <= types::Verb::Sell && order->orderType <= types::OrderType::Stop &&
               order->tif <= types::TimeInForce::None;
    }

} // namespace engine::core
