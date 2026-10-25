#include <gtest/gtest.h>

#include <vector>

#include "OrderBook.hpp"

using namespace engine;
using namespace engine::book;
using engine::core::TradeEvent;

namespace
{
    constexpr types::ClientId CLIENT = 7;

    class OrderBookTest : public ::testing::Test
    {
    protected:
        std::vector<TradeEvent> events;
        FastBook book{
            ArrayBitMapLocator(PriceRange{.minPrice = 10000, .maxPrice = 20000, .tickSize = 5}),
            1,
            [this](const TradeEvent& ev)
            {
                events.push_back(ev);
            }
        };

        void addLimit(types::OrderId id, types::Verb side, types::Price price, types::Quantity qty)
        {
            book.addOrder(
                {.orderId = id,
                 .clientId = CLIENT,
                 .side = side,
                 .orderType = types::OrderType::Limit,
                 .tif = types::TimeInForce::GTC,
                 .limitPrice = price,
                 .qty = qty}
            );
        }

        // Sends a 0-quantity buy against a resting sell and checks that it is rejected
        // without touching the book.
        void expectZeroQtyBuyRejected(
            types::OrderType orderType,
            types::TimeInForce tif,
            types::Price limitPrice
        )
        {
            addLimit(1, types::Verb::Sell, 10100, 10);
            const uint32_t freeCount = book.poolFreeCount();
            events.clear();

            book.addOrder(
                {.orderId = 2,
                 .clientId = CLIENT,
                 .side = types::Verb::Buy,
                 .orderType = orderType,
                 .tif = tif,
                 .limitPrice = limitPrice,
                 .qty = 0}
            );

            ASSERT_EQ(events.size(), 1u);
            EXPECT_EQ(events[0].type, TradeEvent::Type::OrderRejected);
            EXPECT_EQ(events[0].aggressorOrderId, 2u);
            EXPECT_EQ(book.bestBid(), NO_PRICE);
            EXPECT_EQ(book.bestAsk(), 10100u);
            EXPECT_EQ(book.poolFreeCount(), freeCount);

            // The resting sell still has its full quantity.
            events.clear();
            addLimit(3, types::Verb::Buy, 10100, 10);

            ASSERT_EQ(events.size(), 1u);
            EXPECT_EQ(events[0].type, TradeEvent::Type::Fill);
            EXPECT_EQ(events[0].passiveOrderId, 1u);
            EXPECT_EQ(events[0].fillQty, 10u);
            EXPECT_EQ(book.bestAsk(), NO_PRICE);
        }
    };

    TEST_F(OrderBookTest, ZeroQtyMarketOrderIsRejected)
    {
        expectZeroQtyBuyRejected(types::OrderType::Market, types::TimeInForce::IOC, NO_PRICE);
    }

    TEST_F(OrderBookTest, ZeroQtyLimitGtcOrderIsRejected)
    {
        expectZeroQtyBuyRejected(types::OrderType::Limit, types::TimeInForce::GTC, 10100);
    }

    TEST_F(OrderBookTest, ZeroQtyLimitIocOrderIsRejected)
    {
        expectZeroQtyBuyRejected(types::OrderType::Limit, types::TimeInForce::IOC, 10100);
    }

    TEST_F(OrderBookTest, ZeroQtyLimitFokOrderIsRejected)
    {
        expectZeroQtyBuyRejected(types::OrderType::Limit, types::TimeInForce::FOK, 10100);
    }

    TEST_F(OrderBookTest, ModifyToZeroQtyAtSamePriceIsRejected)
    {
        addLimit(1, types::Verb::Sell, 10100, 10);
        events.clear();

        book.modifyOrder({.orderId = 1, .clientId = CLIENT, .newLimitPrice = 10100, .newQty = 0});

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderRejected);
        EXPECT_EQ(events[0].aggressorOrderId, 1u);

        // The order still rests with its full quantity.
        events.clear();
        addLimit(2, types::Verb::Buy, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::Fill);
        EXPECT_EQ(events[0].passiveOrderId, 1u);
        EXPECT_EQ(events[0].fillQty, 10u);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifyToZeroQtyAtNewPriceIsRejected)
    {
        addLimit(1, types::Verb::Sell, 10100, 10);
        events.clear();

        book.modifyOrder({.orderId = 1, .clientId = CLIENT, .newLimitPrice = 10200, .newQty = 0});

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderRejected);
        EXPECT_EQ(events[0].aggressorOrderId, 1u);
        EXPECT_EQ(book.bestAsk(), 10100u);

        // The order still rests at its old price with its full quantity.
        events.clear();
        addLimit(2, types::Verb::Buy, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::Fill);
        EXPECT_EQ(events[0].passiveOrderId, 1u);
        EXPECT_EQ(events[0].fillPrice, 10100u);
        EXPECT_EQ(events[0].fillQty, 10u);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifyWithSamePriceAndQtyKeepsTimePriority)
    {
        addLimit(1, types::Verb::Sell, 10100, 10);
        addLimit(2, types::Verb::Sell, 10100, 10);
        events.clear();

        book.modifyOrder({.orderId = 1, .clientId = CLIENT, .newLimitPrice = 10100, .newQty = 10});

        EXPECT_TRUE(events.empty());

        // Order 1 is still first in the queue.
        addLimit(3, types::Verb::Buy, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::Fill);
        EXPECT_EQ(events[0].passiveOrderId, 1u);
        EXPECT_EQ(events[0].fillQty, 10u);
        EXPECT_EQ(book.bestAsk(), 10100u);
    }
} // namespace
