#pragma once
#include <functional>

#include "ArrayBitMapLocator.hpp"
#include "Command.hpp"
#include "OrderIndex.hpp"
#include "OrderPool.hpp"
#include "PriceLevel.hpp"
#include "TradeEvent.hpp"
#include "Types.hpp"

namespace engine::book
{
    using EventCallback = std::function<void(const core::TradeEvent&)>;

    template <typename LevelLocator, size_t PoolCap = 32768>
    class OrderBook
    {
        static_assert((PoolCap & (PoolCap - 1)) == 0, "PoolCap must be a power of two");

    private:
        OrderPool<PoolCap> pool_;
        LevelLocator locator_;
        OrderIndex<PoolCap * 2> index_;
        types::InstrumentId instrumentId_ = 0;
        EventCallback onEvent_;

        enum class MatchEndReason : uint8_t
        {
            Filled,
            NoLiquidity,
            SelfTradePrevention,
        };

        void emit(engine::core::TradeEvent ev) noexcept
        {
            if (onEvent_)
            {
                onEvent_(ev);
            }
        }

        // Cancel Resting: cancelledBy is the aggressor, cancelled is the passive
        // Cancel Aggressor: cancelledBy is 0, cancelled is the aggressor
        void emitSelfTradeCancelled(const Order& cancelled, types::OrderId cancelledBy = 0) noexcept
        {
            emit(
                {.type = engine::core::TradeEvent::Type::SelfTradeCancelled,
                 .instrumentId = instrumentId_,
                 .aggressorOrderId = cancelled.orderId,
                 .passiveOrderId = cancelledBy,
                 .aggressorClientId = cancelled.clientId,
                 .aggressorRemaining = cancelled.qty}
            );
        }

        [[nodiscard]] bool canFullyFill(const engine::core::NewOrder& order) const noexcept
        {
            const bool isBuy = (order.side == types::Verb::Buy);
            const types::Verb passiveSide = isBuy ? types::Verb::Sell : types::Verb::Buy;

            types::Quantity remainingQty = order.qty;
            types::Price bestPrice = isBuy ? locator_.bestAsk() : locator_.bestBid();

            while (remainingQty > 0 && bestPrice != NO_PRICE)
            {
                if ((isBuy && bestPrice > order.limitPrice) ||
                    (!isBuy && bestPrice < order.limitPrice))
                {
                    break;
                }

                const PriceLevel& level = locator_.level(passiveSide, bestPrice);

                uint32_t slotIdx = level.head;
                while (remainingQty > 0 && slotIdx != NULL_IDX)
                {
                    const PoolSlot& passiveSlot = pool_[slotIdx];
                    const Order& passiveOrder = passiveSlot.order();

                    // STP
                    if (passiveOrder.clientId == order.clientId)
                    {
                        switch (order.stp)
                        {
                        case types::SelfTradePrevention::CancelResting:
                            // Move on to the next passive order
                            break;
                        case types::SelfTradePrevention::CancelAggressor:
                            return false;
                        }
                    }
                    else
                    {
                        remainingQty -= std::min(remainingQty, passiveOrder.qty);
                    }

                    slotIdx = passiveSlot.next();
                }

                bestPrice = isBuy ? locator_.nextAsk(bestPrice) : locator_.nextBid(bestPrice);
            }

            return remainingQty == 0;
        }

        // Matches the order against the book and returns why it stopped. Writes the order's
        // qty as the quantity still open. Raises appropriate events for the passive orders
        MatchEndReason matchAggressor(Order& order) noexcept
        {
            const bool isBuy = (order.verb == types::Verb::Buy);
            const types::Verb passiveSide = isBuy ? types::Verb::Sell : types::Verb::Buy;

            while (order.qty > 0)
            {
                const types::Price bestPrice = isBuy ? locator_.bestAsk() : locator_.bestBid();
                if (bestPrice == NO_PRICE)
                {
                    return MatchEndReason::NoLiquidity;
                }
                if (order.price != NO_PRICE)
                {
                    // if no order that can match my limit price
                    if ((isBuy && bestPrice > order.price) || (!isBuy && bestPrice < order.price))
                    {
                        return MatchEndReason::NoLiquidity;
                    }
                }

                const PriceLevel& level = locator_.level(passiveSide, bestPrice);
                while (order.qty > 0 && !level.empty())
                {
                    const uint32_t passiveSlotIdx = level.head;
                    const Order& passiveOrder = pool_[passiveSlotIdx].order();

                    if (passiveOrder.clientId == order.clientId)
                    {
                        switch (order.stp)
                        {
                        case types::SelfTradePrevention::CancelResting:
                            emitSelfTradeCancelled(passiveOrder, order.orderId);
                            removeFromBook(passiveSlotIdx);
                            continue;
                        case types::SelfTradePrevention::CancelAggressor:
                            return MatchEndReason::SelfTradePrevention;
                        }
                    }

                    const types::Quantity fillQty = std::min(order.qty, passiveOrder.qty);
                    const bool passiveFilled = (fillQty == passiveOrder.qty);

                    order.qty -= fillQty;
                    if (!passiveFilled)
                    {
                        locator_.reduce(passiveSlotIdx, fillQty, pool_.slots());
                    }

                    emit({
                        .type = passiveFilled ? engine::core::TradeEvent::Type::Fill
                                              : engine::core::TradeEvent::Type::PartialFill,
                        .instrumentId = instrumentId_,
                        .aggressorOrderId = order.orderId,
                        .passiveOrderId = passiveOrder.orderId,
                        .aggressorClientId = order.clientId,
                        .passiveClientId = passiveOrder.clientId,
                        .fillPrice = bestPrice,
                        .fillQty = fillQty,
                        .aggressorRemaining = order.qty,
                        .passiveRemaining = passiveFilled ? 0 : passiveOrder.qty,
                    });

                    if (passiveFilled)
                    {
                        removeFromBook(passiveSlotIdx);
                    }
                }
            }

            return MatchEndReason::Filled;
        }

        bool restInBook(const Order& resting) noexcept
        {
            const uint32_t slotIdx = pool_.acquire(resting);
            if (__builtin_expect(slotIdx == NULL_IDX, 0))
            {
                return false;
            }

            locator_.append(slotIdx, pool_.slots());
            index_.insert(resting.orderId, slotIdx);

            emit(
                {.type = engine::core::TradeEvent::Type::OrderAccepted,
                 .instrumentId = instrumentId_,
                 .aggressorOrderId = resting.orderId,
                 .aggressorClientId = resting.clientId,
                 .aggressorRemaining = resting.qty}
            );

            return true;
        }

        // Takes a resting order out of its level, removes it from the index, then frees its slot.
        void removeFromBook(uint32_t slotIdx) noexcept
        {
            locator_.remove(slotIdx, pool_.slots());
            index_.erase(pool_[slotIdx].order().orderId);
            pool_.release(slotIdx);
        }

        // Moves a resting order to a new price or a higher quantity, keeping its slot and index
        // entry. It matches first if the new price crosses, and only what is left rests, unless
        // self-trade prevention cancels it.
        void moveOrder(uint32_t slotIdx, types::Price newPrice, types::Quantity newQty) noexcept
        {
            locator_.remove(slotIdx, pool_.slots());

            Order moved = pool_[slotIdx].order();
            moved.price = newPrice;
            moved.qty = newQty;

            switch (matchAggressor(moved))
            {
            case MatchEndReason::NoLiquidity:
                pool_.reprice(slotIdx, moved.price, moved.qty);
                locator_.append(slotIdx, pool_.slots());
                break;
            case MatchEndReason::SelfTradePrevention:
                emitSelfTradeCancelled(moved);
                [[fallthrough]];
            case MatchEndReason::Filled:
                index_.erase(moved.orderId);
                pool_.release(slotIdx);
                break;
            }
        }

    public:
        explicit OrderBook(
            LevelLocator locator,
            types::InstrumentId instrumentId = 0,
            EventCallback cb = nullptr
        )
            : locator_(std::move(locator)),
              instrumentId_(instrumentId),
              onEvent_(std::move(cb))
        {
        }

        OrderBook() = default;
        OrderBook(const OrderBook&) = delete;
        OrderBook& operator=(const OrderBook&) = delete;
        OrderBook(OrderBook&&) noexcept = default;
        // Pool slots cannot be assigned, so neither can a book.
        OrderBook& operator=(OrderBook&&) = delete;

        void addOrder(const engine::core::NewOrder& order) noexcept
        {
            if (order.qty == 0)
            {
                emit(
                    {.type = engine::core::TradeEvent::Type::OrderRejected,
                     .instrumentId = instrumentId_,
                     .aggressorOrderId = order.orderId,
                     .aggressorClientId = order.clientId}
                );

                return;
            }

            Order incoming{
                .orderId = order.orderId,
                .verb = order.side,
                .clientId = order.clientId,
                .price = order.limitPrice,
                .qty = order.qty,
                .tif = order.tif,
                .stp = order.stp,
            };

            if (order.orderType == types::OrderType::Market)
            {
                switch (matchAggressor(incoming))
                {
                case MatchEndReason::Filled:
                    break;
                case MatchEndReason::NoLiquidity:
                    emit(
                        {.type = engine::core::TradeEvent::Type::OrderRejected,
                         .instrumentId = instrumentId_,
                         .aggressorOrderId = order.orderId,
                         .aggressorClientId = order.clientId,
                         .aggressorRemaining = incoming.qty}
                    );
                    break;
                case MatchEndReason::SelfTradePrevention:
                    emitSelfTradeCancelled(incoming);
                    break;
                }

                return;
            }

            // Validate price before touching the book.
            if (!locator_.isValidPrice(order.limitPrice))
            {
                emit(
                    {.type = engine::core::TradeEvent::Type::OrderRejected,
                     .instrumentId = instrumentId_,
                     .aggressorOrderId = order.orderId,
                     .aggressorClientId = order.clientId}
                );

                return;
            }

            // FOK
            if (order.tif == types::TimeInForce::FOK && !canFullyFill(order))
            {
                emit(
                    {.type = engine::core::TradeEvent::Type::OrderRejected,
                     .instrumentId = instrumentId_,
                     .aggressorOrderId = order.orderId,
                     .aggressorClientId = order.clientId}
                );

                return;
            }

            switch (matchAggressor(incoming))
            {
            case MatchEndReason::Filled:
                break;
            case MatchEndReason::NoLiquidity:
                if (order.tif == types::TimeInForce::IOC)
                {
                    return; // discard unfilled
                }
                restInBook(incoming);
                break;
            case MatchEndReason::SelfTradePrevention:
                emitSelfTradeCancelled(incoming);
                break;
            }
        }

        void cancelOrder(const engine::core::CancelOrder& request) noexcept
        {
            uint32_t* it = index_.find(request.orderId);
            if (it == nullptr)
            {
                emit(
                    {.type = engine::core::TradeEvent::Type::OrderRejected,
                     .instrumentId = instrumentId_,
                     .aggressorOrderId = request.orderId,
                     .aggressorClientId = request.clientId}
                );

                return;
            }
            const uint32_t slotIdx = *it;

            emit(
                {.type = engine::core::TradeEvent::Type::OrderCancelled,
                 .instrumentId = instrumentId_,
                 .aggressorOrderId = request.orderId,
                 .aggressorClientId = pool_[slotIdx].order().clientId}
            );

            removeFromBook(slotIdx);
        }

        void modifyOrder(const engine::core::ModifyOrder& request) noexcept
        {
            uint32_t* it = index_.find(request.orderId);
            if (it == nullptr)
            {
                emit(
                    {.type = engine::core::TradeEvent::Type::OrderRejected,
                     .instrumentId = instrumentId_,
                     .aggressorOrderId = request.orderId,
                     .aggressorClientId = request.clientId}
                );

                return;
            }

            if (!locator_.isValidPrice(request.newLimitPrice) || request.newQty == 0)
            {
                emit(
                    {.type = engine::core::TradeEvent::Type::OrderRejected,
                     .instrumentId = instrumentId_,
                     .aggressorOrderId = request.orderId,
                     .aggressorClientId = request.clientId}
                );

                return;
            }

            const uint32_t slotIdx = *it;
            const Order& order = pool_[slotIdx].order();

            // Same price and quantity: order keeps its place.
            if (request.newLimitPrice == order.price && request.newQty == order.qty)
            {
                return;
            }

            // Quantity decrease at same price: update in place, preserve time priority.
            if (request.newLimitPrice == order.price && request.newQty < order.qty)
            {
                locator_.reduce(slotIdx, order.qty - request.newQty, pool_.slots());
            }
            else
            {
                // Price change or quantity increase
                moveOrder(slotIdx, request.newLimitPrice, request.newQty);
            }
        }

        [[nodiscard]] types::Price bestBid() const noexcept
        {
            return locator_.bestBid();
        }

        [[nodiscard]] types::Price bestAsk() const noexcept
        {
            return locator_.bestAsk();
        }

        [[nodiscard]] uint32_t poolFreeCount() const noexcept
        {
            return pool_.freeCount();
        }

        void setEventCallback(EventCallback cb) noexcept
        {
            onEvent_ = std::move(cb);
        }
    };

    using FastBook = OrderBook<ArrayBitMapLocator, 32768>;
} // namespace engine::book