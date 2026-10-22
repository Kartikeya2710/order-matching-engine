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

        [[nodiscard]] bool canFullyFill(const engine::core::NewOrder& order) const noexcept
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

                const PriceLevel& level = locator_.level(passiveSide, bestPrice);

                uint32_t slot = level.head;
                while (remainingQty > 0 && slot != NULL_IDX)
                {
                    const PoolSlot& passiveSlot = pool_[slot];
                    remainingQty -= std::min(remainingQty, passiveSlot.order().qty);
                    slot = passiveSlot.next();
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
                const types::Price bestPrice = isBuy ? locator_.bestAsk() : locator_.bestBid();
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

                const PriceLevel& level = locator_.level(passiveSide, bestPrice);
                while (remainingQty > 0 && !level.empty())
                {
                    const uint32_t passiveSlot = level.head;
                    const Order& passiveOrder = pool_[passiveSlot].order();
                    const types::Quantity fillQty = std::min(remainingQty, passiveOrder.qty);
                    const bool passiveFilled = (fillQty == passiveOrder.qty);

                    remainingQty -= fillQty;
                    if (!passiveFilled)
                    {
                        locator_.reduce(passiveSlot, fillQty, pool_.slots());
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
                        .aggressorRemaining = remainingQty,
                        .passiveRemaining = passiveFilled ? 0 : passiveOrder.qty,
                    });

                    if (passiveFilled)
                    {
                        removeFromBook(passiveSlot);
                    }
                }
            }

            return remainingQty;
        }

        bool restInBook(const Order& resting) noexcept
        {
            const uint32_t slot = pool_.acquire(resting);
            if (__builtin_expect(slot == NULL_IDX, 0))
            {
                return false;
            }

            locator_.append(slot, pool_.slots());
            index_.insert(resting.orderId, slot);

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
        void removeFromBook(uint32_t slot) noexcept
        {
            locator_.remove(slot, pool_.slots());
            index_.erase(pool_[slot].order().orderId);
            pool_.release(slot);
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
            const uint32_t slot = *it;

            emit(
                {.type = engine::core::TradeEvent::Type::OrderCancelled,
                 .instrumentId = instrumentId_,
                 .aggressorOrderId = request.orderId,
                 .aggressorClientId = pool_[slot].order().clientId}
            );

            removeFromBook(slot);
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

            const uint32_t slot = *it;
            const Order& order = pool_[slot].order();

            // Quantity decrease at same price: update in place, preserve time priority.
            if (request.newLimitPrice == order.price && request.newQty < order.qty)
            {
                locator_.reduce(slot, order.qty - request.newQty, pool_.slots());
            }
            else
            {
                // Everything else (price change or quantity increase): cancel + re-add.
                // Side, owner and TIF are copied from the resting order; a modify cannot change
                // them.
                Order replacement = order;
                replacement.price = request.newLimitPrice;
                replacement.qty = request.newQty;

                removeFromBook(slot);
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