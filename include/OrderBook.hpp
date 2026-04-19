#pragma once
#include <functional>
#include <unordered_map>
#include "Command.hpp"
#include "OrderPool.hpp"
#include "TradeEvent.hpp"
#include "PriceLevel.hpp"
#include "ArrayBitMapLocator.hpp"
#include "OrderIndex.hpp"

namespace engine::book
{
    using EventCallback = std::function<void(const core::TradeEvent &)>;

    template <
        typename LevelLocator,
        size_t PoolCap = 32768>
    class OrderBook
    {
        static_assert((Cap & (Cap - 1)) == 0, "Cap must be a power of two");

    private:
        OrderPool<PoolCap> pool_;
        LevelLocator locator_;
        OrderIndex<PoolCap * 2> index_;
        types::InstrumentId instrumentId_ = 0;
        EventCallback onEvent_;

        void emit(engine::core::TradeEvent ev) noexcept
        {
            if (onEvent_)
                onEvent_(ev);
        }

        // Run the match loop. Returns unfilled remainingQty quantity.
        // priceLimit is ignored for market orders (pass NO_PRICE).
        types::Quantity matchAggressor(const engine::core::Command &cmd) noexcept
        {
            const bool isBuy = (cmd.verb == types::Verb::Buy);
            const types::Verb passiveSide = isBuy ? types::Verb::Sell : types::Verb::Buy;
            types::Quantity remainingQty = cmd.qty;

            while (remainingQty > 0)
            {
                types::Price bestPrice = isBuy ? locator_.bestAsk() : locator_.bestBid();
                if (bestPrice == NO_PRICE)
                    break;
                if (cmd.limitPrice != NO_PRICE)
                {
                    // if no order that can match my limit price
                    if ((isBuy && bestPrice > cmd.limitPrice) || (!isBuy && bestPrice < cmd.limitPrice))
                        break;
                }

                PriceLevel &level = locator_.getPriceLevel(passiveSide, bestPrice);
                // !!!!!!!!!!!!!!!!! CATASTRPHIC FAILURE !!!!!!!!!!!!!!!!!!!!
                if (level.empty())
                    break; // should not happen if bitmap is correct

                while (remainingQty > 0 && !level.empty())
                {
                    uint32_t passivePoolIdx = level.head;
                    PoolOrder &passiveOrder = pool_[passivePoolIdx];
                    types::Quantity fillQty = std::min(remainingQty, passiveOrder.qty);

                    // trade happens

                    remainingQty -= fillQty;
                    passiveOrder.qty -= fillQty;
                    level.totalQty -= fillQty;

                    bool fulfill = passiveOrder.qty == 0;
                    emit({
                        .type = fulfill
                                    ? engine::core::TradeEvent::Type::Fill
                                    : engine::core::TradeEvent::Type::PartialFill,
                        .instrumentId = instrumentId_,
                        .aggressorOrderId = cmd.orderId,
                        .passiveOrderId = passiveOrder.orderId,
                        .aggressorClientId = cmd.clientId,
                        .passiveClientId = passiveOrder.clientId,
                        .fillPrice = bestPrice,
                        .fillQty = fillQty,
                        .aggressorRemaining = remainingQty,
                        .passiveRemaining = passiveOrder.qty,
                    });

                    // exhausted the passive order
                    if (fulfill)
                    {
                        removeFromBook(passiveOrder.orderId, passivePoolIdx, passiveSide, bestPrice);
                    }
                }
            }
            return remainingQty;
        }

        bool restInBook(const engine::core::Command &cmd, types::Quantity qty) noexcept
        {
            uint32_t poolIdx = pool_.acquire();
            if (__builtin_expect(poolIdx == UINT32_MAX, 0))
            {
                return false;
            }

            pool_[poolIdx] = PoolOrder{
                .orderId = cmd.orderId,
                .verb = cmd.verb,
                .clientId = cmd.clientId,
                .price = cmd.limitPrice,
                .qty = qty,
                .tif = cmd.tif,
                .next = NULL_IDX,
                .prev = NULL_IDX,
            };

            PriceLevel &level = locator_.getPriceLevel(cmd.verb, cmd.limitPrice);
            appendOrderInLevel(level, poolIdx, pool_);
            locator_.markNonEmpty(cmd.verb, cmd.limitPrice);

            index_.insert([cmd.orderId], poolIdx);

            emit({.type = engine::core::TradeEvent::Type::OrderAccepted,
                  .instrumentId = instrumentId_,
                  .aggressorOrderId = cmd.orderId,
                  .aggressorClientId = cmd.clientId,
                  .aggressorRemaining = qty});

            return true;
        }

        // Removes an order from its price level and releases its pool slot.
        // Does NOT touch the index — callers handle that.
        void removeFromBook(types::OrderId orderId, uint32_t poolIdx,
                            types::Verb verb, types::Price price) noexcept
        {
            PriceLevel &level = locator_.getPriceLevel(verb, price);
            removeOrderFromLevel(level, poolIdx, pool_);

            if (level.empty())
            {
                locator_.markEmpty(verb, price);
            }

            pool_.release(poolIdx);
            index_.erase(orderId);
        }

    public:
        explicit OrderBook(LevelLocator locator,
                           types::InstrumentId instrumentId = 0,
                           EventCallback cb = nullptr)
            : locator_(std::move(locator)), instrumentId_(instrumentId), onEvent_(std::move(cb))
        {
        }

        OrderBook() = default;
        OrderBook(const OrderBook &) = delete;
        OrderBook &operator=(const OrderBook &) = delete;
        OrderBook(OrderBook &&) noexcept = default;
        OrderBook &operator=(OrderBook &&) noexcept = default;

        void addOrder(const engine::core::Command &cmd) noexcept
        {
            if (cmd.orderType == types::OrderType::Market)
            {
                types::Quantity remainingQty = matchAggressor(cmd);
                if (remainingQty > 0)
                {
                    emit({.type = engine::core::TradeEvent::Type::OrderRejected,
                          .instrumentId = instrumentId_,
                          .aggressorOrderId = cmd.orderId,
                          .aggressorClientId = cmd.clientId,
                          .aggressorRemaining = remainingQty});
                }

                return;
            }

            // Validate price before touching the book.
            if (!locator_.isInRange(cmd.limitPrice) || !locator_.isAligned(cmd.limitPrice))
            {
                emit({.type = engine::core::TradeEvent::Type::OrderRejected,
                      .instrumentId = instrumentId_,
                      .aggressorOrderId = cmd.orderId,
                      .aggressorClientId = cmd.clientId});

                return;
            }

            types::Quantity remainingQty = matchAggressor(cmd);

            if (remainingQty == 0)
                return; // fully filled
            if (cmd.tif == types::TimeInForce::IOC)
                return; // discard unfilled

            restInBook(cmd, remainingQty);
        }

        void cancelOrder(const engine::core::Command &cmd) noexcept
        {
            uint32_t *it = index_.find(cmd.orderId);
            if (it == nullptr)
            {
                emit({.type = engine::core::TradeEvent::Type::OrderRejected,
                      .instrumentId = instrumentId_,
                      .aggressorOrderId = cmd.orderId,
                      .aggressorClientId = cmd.clientId});
                return;
            }
            uint32_t poolIdx = *it;
            PoolOrder &order = pool_[poolIdx]; // get stored verb/price, NOT cmd fields

            emit({.type = engine::core::TradeEvent::Type::OrderCancelled,
                  .instrumentId = instrumentId_,
                  .aggressorOrderId = cmd.orderId,
                  .aggressorClientId = order.clientId});

            removeFromBook(order.orderId, poolIdx, order.verb, order.price);
        }

        void modifyOrder(const engine::core::Command &cmd) noexcept
        {
            uint32_t *it = index_.find(cmd.orderId);
            if (it == nullptr)
            {
                emit({.type = engine::core::TradeEvent::Type::OrderRejected,
                      .instrumentId = instrumentId_,
                      .aggressorOrderId = cmd.orderId,
                      .aggressorClientId = cmd.clientId});

                return;
            }

            uint32_t poolIdx = *it;
            PoolOrder &order = pool_[poolIdx];

            // Quantity decrease at same price: update in place, preserve time priority.
            if (cmd.limitPrice == order.price && cmd.verb == order.verb && cmd.qty < order.qty)
            {
                PriceLevel &level = locator_.getPriceLevel(order.verb, order.price);
                level.totalQty -= (order.qty - cmd.qty);
                order.qty = cmd.qty;
                return;
            }

            // Everything else (price change or quantity increase): cancel + re-add.
            removeFromBook(order.orderId, poolIdx, order.verb, order.price);
            restInBook(cmd, cmd.qty);
        }

        types::Price bestBid() const noexcept { return locator_.bestBid(); }
        types::Price bestAsk() const noexcept { return locator_.bestAsk(); }
        uint32_t poolFreeCount() const noexcept { return pool_.freeCount(); }
        void setEventCallback(EventCallback cb) noexcept { onEvent_ = std::move(cb); }
    };

    using FastBook = OrderBook<ArrayBitMapLocator, 32768>;
}