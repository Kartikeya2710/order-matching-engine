#pragma once
#include <string>
#include <vector>
#include <deque>
#include <map>
#include "Command.hpp"
#include "OrderPool.hpp"
#include "PriceLevel.hpp"
#include "ArrayBitMapLocator.hpp"

namespace engine::book
{
    template <
        typename LevelLocator,
        size_t PoolCap = 32768,
        size_t IndexCap = 65536>
    class OrderBook
    {
    private:
        OrderPool<PoolCap> pool_;
        LevelLocator locator_;
        std::unordered_map<types::OrderId, std::uint32_t> index_;

        types::InstrumentId instrumentId_;
        std::size_t capacity_;

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
                    break;

                while (remainingQty > 0 && !level.empty())
                {
                    std::uint32_t passivePoolIdx = level.head;
                    PoolOrder &passiveOrder = pool_[passivePoolIdx];
                    types::Quantity fillQty = std::min(remainingQty, passiveOrder.qty);

                    // trade happens

                    remainingQty -= fillQty;
                    passiveOrder.qty -= fillQty;

                    // exhausted the passive order
                    if (passiveOrder.qty == 0)
                    {
                        removeFromBook(cmd.orderId, passivePoolIdx, passiveSide, bestPrice);
                    }
                }
            }

            return remainingQty;
        }

        bool restInBook(const engine::core::Command &cmd, types::Quantity qty) noexcept
        {
            std::uint32_t poolIdx = pool_.acquire();
            if (__builtin_expect(poolIdx == NULL_IDX, 0))
            {
                // emit rejection
                return false;
            }

            pool_[poolIdx] = PoolOrder{
                .orderId = cmd.orderId,
                .clientId = cmd.clientId,
                .price = cmd.limitPrice,
                .qty = qty,
                .verb = cmd.verb,
                .tif = cmd.tif,
                .next = NULL_IDX,
                .prev = NULL_IDX,
            };

            PriceLevel &level = locator_.getPriceLevel(cmd.verb, cmd.limitPrice);
            appendOrderInLevel(level, poolIdx, pool_);
            locator_.markNonEmpty(cmd.verb, cmd.limitPrice);

            index_[cmd.orderId] = poolIdx;

            return true;
        }

        void removeFromBook(types::OrderId orderId, std::uint32_t poolIdx, types::Verb verb, types::Price price) noexcept
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

        static void emitRejected(
            const engine::core::Command &cmd,
            [[maybe_unused]] const char *reason) noexcept
        {
        }

    public:
        explicit OrderBook(LevelLocator locator) : locator_(std::move(locator))
        {
        }

        OrderBook() = default;

        OrderBook(const OrderBook &) = delete;
        OrderBook &operator=(const OrderBook &) = delete;

        void addOrder(const engine::core::Command &cmd) noexcept
        {
            const bool isBuy = (cmd.verb == types::Verb::Buy);
            const auto oppSide = isBuy ? types::Verb::Sell : types::Verb::Buy;
            types::Quantity remainingQty = cmd.qty;

            if (cmd.orderType == types::OrderType::Market)
            {
                remainingQty = matchAggressor(cmd);
                if (remainingQty > 0)
                {
                    emitRejected(cmd, "market order unfilled");
                }
                return;
            }

            remainingQty = matchAggressor(cmd);

            if (remainingQty == 0)
            {
                return;
            }
            if (cmd.tif == types::TimeInForce::IOC)
            {
                return;
            }

            restInBook(cmd, remainingQty);
        }

        void cancelOrder(const engine::core::Command &cmd) noexcept
        {
            auto it = index_.find(cmd.orderId);
            if (it == index_.end())
            {
                // reject the request
                return;
            }

            PoolOrder &order = pool_[it->second];
            removeFromBook(cmd.orderId, it->second, order.verb, order.price);
        }

        void modifyOrder(const engine::core::Command &cmd) noexcept
        {
            auto it = index_.find(cmd.orderId);
            if (it == index_.end())
            {
                // reject the request
                return;
            }
            std::uint32_t poolIdx = it->second;
            PoolOrder &order = pool_[poolIdx];

            // Price unchanged and quantity reduced (the only case where I preserve price-time priority)
            if (cmd.limitPrice == order.price && cmd.verb == order.verb && cmd.qty < order.qty)
            {
                PriceLevel &level = locator_.getPriceLevel(order.verb, order.price);
                level.totalQty -= (order.qty - cmd.qty);
                order.qty = cmd.qty;
                return;
            }

            // Remove the order and insert a new one
            removeFromBook(order.orderId, poolIdx, order.verb, order.price);
            restInBook(cmd, cmd.qty);
        }

        types::Price bestBid() const noexcept { return locator_.bestBid(); }
        types::Price bestAsk() const noexcept { return locator_.bestAsk(); }

        size_t poolFreeCount() const noexcept { return pool_.freeCount(); }
    };

    using FastBook = OrderBook<ArrayBitMapLocator, 32768, 65536>;
}
