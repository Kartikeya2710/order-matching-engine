#pragma once
#include <functional>

#include "ArrayBitMapLocator.hpp"
#include "Command.hpp"
#include "OrderIndex.hpp"
#include "OrderPool.hpp"
#include "PriceLevel.hpp"
#include "TradeEvent.hpp"

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

        void emit(engine::core::TradeEvent ev) noexcept
        {
            if (onEvent_)
            {
                onEvent_(ev);
            }
        }

        bool canFullyFill(const engine::core::NewOrder& order) noexcept
        {
            const bool isBuy = (order.side == types::Verb::Buy);
            const types::Verb passiveSide = isBuy ? types::Verb::Sell : types::Verb::Buy;
            types::Quantity remainingQty = order.qty;

            types::Price bestPrice = isBuy ? locator_.bestAsk() : locator_.bestBid();

            while (remainingQty > 0 && bestPrice != NO_PRICE)
            {
                if (order.limitPrice != NO_PRICE)
                {
                    if ((isBuy && bestPrice > order.limitPrice) ||
                        (!isBuy && bestPrice < order.limitPrice))
                    {
                        break;
                    }
                }

                PriceLevel& level = locator_.getPriceLevel(passiveSide, bestPrice);

                uint32_t idx = level.head;
                while (remainingQty > 0 && idx != NULL_IDX)
                {
                    PoolOrder& passiveOrder = pool_[idx];
                    remainingQty -= std::min(remainingQty, passiveOrder.qty);
                    idx = passiveOrder.next;
                }

                bestPrice = isBuy ? locator_.nextAsk(bestPrice) : locator_.nextBid(bestPrice);
            }

            return remainingQty == 0;
        }

        // Run the match loop. Returns unfilled remainingQty quantity.
        // priceLimit is ignored for market orders (pass NO_PRICE).
        types::Quantity matchAggressor(const engine::core::NewOrder& order) noexcept
        {
            const bool isBuy = (order.side == types::Verb::Buy);
            const types::Verb passiveSide = isBuy ? types::Verb::Sell : types::Verb::Buy;
            types::Quantity remainingQty = order.qty;

            while (remainingQty > 0)
            {
                types::Price bestPrice = isBuy ? locator_.bestAsk() : locator_.bestBid();
                if (bestPrice == NO_PRICE)
                {
                    break;
                }
                if (order.limitPrice != NO_PRICE)
                {
                    // if no order that can match my limit price
                    if ((isBuy && bestPrice > order.limitPrice) ||
                        (!isBuy && bestPrice < order.limitPrice))
                    {
                        break;
                    }
                }

                PriceLevel& level = locator_.getPriceLevel(passiveSide, bestPrice);
                while (remainingQty > 0 && !level.empty())
                {
                    uint32_t passivePoolIdx = level.head;
                    PoolOrder& passiveOrder = pool_[passivePoolIdx];
                    types::Quantity fillQty = std::min(remainingQty, passiveOrder.qty);

                    // trade happens

                    remainingQty -= fillQty;
                    passiveOrder.qty -= fillQty;
                    level.totalQty -= fillQty;

                    bool fulfill = passiveOrder.qty == 0;
                    emit({
                        .type = fulfill ? engine::core::TradeEvent::Type::Fill
                                        : engine::core::TradeEvent::Type::PartialFill,
                        .instrumentId = instrumentId_,
                        .aggressorOrderId = order.orderId,
                        .passiveOrderId = passiveOrder.orderId,
                        .aggressorClientId = order.clientId,
                        .passiveClientId = passiveOrder.clientId,
                        .fillPrice = bestPrice,
                        .fillQty = fillQty,
                        .aggressorRemaining = remainingQty,
                        .passiveRemaining = passiveOrder.qty,
                    });

                    // exhausted the passive order
                    if (fulfill)
                    {
                        removeFromBook(
                            passiveOrder.orderId,
                            passivePoolIdx,
                            passiveSide,
                            bestPrice
                        );
                    }
                }
            }

            return remainingQty;
        }

        bool restInBook(const PoolOrder& resting) noexcept
        {
            uint32_t poolIdx = pool_.acquire();
            if (__builtin_expect(poolIdx == UINT32_MAX, 0))
            {
                return false;
            }

            pool_[poolIdx] = resting;

            PriceLevel& level = locator_.getPriceLevel(resting.verb, resting.price);
            appendOrderInLevel(level, poolIdx, pool_);
            locator_.markNonEmpty(resting.verb, resting.price);

            index_.insert(resting.orderId, poolIdx);

            emit(
                {.type = engine::core::TradeEvent::Type::OrderAccepted,
                 .instrumentId = instrumentId_,
                 .aggressorOrderId = resting.orderId,
                 .aggressorClientId = resting.clientId,
                 .aggressorRemaining = resting.qty}
            );

            return true;
        }

        // Removes an order from its price level and releases its pool slot.
        // Does NOT touch the index — callers handle that.
        void removeFromBook(
            types::OrderId orderId,
            uint32_t poolIdx,
            types::Verb verb,
            types::Price price
        ) noexcept
        {
            PriceLevel& level = locator_.getPriceLevel(verb, price);
            removeOrderFromLevel(level, poolIdx, pool_);

            if (level.empty())
            {
                locator_.markEmpty(verb, price);
            }

            pool_.release(poolIdx);
            index_.erase(orderId);
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
        OrderBook& operator=(OrderBook&&) noexcept = default;

        void addOrder(const engine::core::NewOrder& order) noexcept
        {
            if (order.orderType == types::OrderType::Market)
            {
                types::Quantity remainingQty = matchAggressor(order);
                if (remainingQty > 0)
                {
                    emit(
                        {.type = engine::core::TradeEvent::Type::OrderRejected,
                         .instrumentId = instrumentId_,
                         .aggressorOrderId = order.orderId,
                         .aggressorClientId = order.clientId,
                         .aggressorRemaining = remainingQty}
                    );
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

            types::Quantity remainingQty = matchAggressor(order);

            if (remainingQty == 0)
            {
                return; // fully filled
            }
            if (order.tif == types::TimeInForce::IOC)
            {
                return; // discard unfilled
            }

            restInBook({
                .orderId = order.orderId,
                .verb = order.side,
                .clientId = order.clientId,
                .price = order.limitPrice,
                .qty = remainingQty,
                .tif = order.tif,
            });
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
            uint32_t poolIdx = *it;
            PoolOrder& order = pool_[poolIdx];

            emit(
                {.type = engine::core::TradeEvent::Type::OrderCancelled,
                 .instrumentId = instrumentId_,
                 .aggressorOrderId = request.orderId,
                 .aggressorClientId = order.clientId}
            );

            removeFromBook(order.orderId, poolIdx, order.verb, order.price);
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

            if (!locator_.isValidPrice(request.newLimitPrice))
            {
                emit(
                    {.type = engine::core::TradeEvent::Type::OrderRejected,
                     .instrumentId = instrumentId_,
                     .aggressorOrderId = request.orderId,
                     .aggressorClientId = request.clientId}
                );

                return;
            }

            uint32_t poolIdx = *it;
            PoolOrder& order = pool_[poolIdx];

            // Quantity decrease at same price: update in place, preserve time priority.
            if (request.newLimitPrice == order.price && request.newQty < order.qty)
            {
                PriceLevel& level = locator_.getPriceLevel(order.verb, order.price);
                level.totalQty -= (order.qty - request.newQty);
                order.qty = request.newQty;
            }
            else
            {
                // Everything else (price change or quantity increase): cancel + re-add.
                // Side, owner and TIF are copied from the resting order; a modify cannot change
                // them.
                PoolOrder replacement = order;
                replacement.price = request.newLimitPrice;
                replacement.qty = request.newQty;

                removeFromBook(order.orderId, poolIdx, order.verb, order.price);
                restInBook(replacement);
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