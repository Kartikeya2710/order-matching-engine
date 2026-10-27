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

        void modify(types::OrderId id, types::Price newPrice, types::Quantity newQty)
        {
            book.modifyOrder(
                {.orderId = id, .clientId = CLIENT, .newLimitPrice = newPrice, .newQty = newQty}
            );
        }

        static void expectFill(
            const TradeEvent& ev,
            types::OrderId aggressorId,
            types::OrderId passiveId,
            types::Price price,
            types::Quantity qty,
            types::Quantity aggressorRemaining
        )
        {
            EXPECT_EQ(ev.aggressorOrderId, aggressorId);
            EXPECT_EQ(ev.passiveOrderId, passiveId);
            EXPECT_EQ(ev.fillPrice, price);
            EXPECT_EQ(ev.fillQty, qty);
            EXPECT_EQ(ev.aggressorRemaining, aggressorRemaining);
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

        // Sends a limit buy at `price` against a resting sell at 15000 and checks that it is
        // rejected without touching the book.
        void expectLimitBuyRejected(types::Price price)
        {
            addLimit(1, types::Verb::Sell, 15000, 10);
            const uint32_t freeCount = book.poolFreeCount();
            events.clear();

            addLimit(2, types::Verb::Buy, price, 10);

            ASSERT_EQ(events.size(), 1u);
            EXPECT_EQ(events[0].type, TradeEvent::Type::OrderRejected);
            EXPECT_EQ(events[0].aggressorOrderId, 2u);
            EXPECT_EQ(book.bestBid(), NO_PRICE);
            EXPECT_EQ(book.bestAsk(), 15000u);
            EXPECT_EQ(book.poolFreeCount(), freeCount);
        }

        // Modifies a resting buy at 15000 to `price` and checks that the modify is rejected and
        // the buy still rests at 15000 with its full quantity.
        void expectModifyRejected(types::Price price)
        {
            addLimit(1, types::Verb::Buy, 15000, 10);
            events.clear();

            modify(1, price, 10);

            ASSERT_EQ(events.size(), 1u);
            EXPECT_EQ(events[0].type, TradeEvent::Type::OrderRejected);
            EXPECT_EQ(events[0].aggressorOrderId, 1u);
            EXPECT_EQ(book.bestBid(), 15000u);

            events.clear();
            addLimit(2, types::Verb::Sell, 15000, 10);

            ASSERT_EQ(events.size(), 1u);
            expectFill(events[0], 2, 1, 15000, 10, 0);
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

    // Prices at the book's boundary. The range is 10000 to 20000 with a tick of 5.

    TEST_F(OrderBookTest, LimitPriceBelowMinIsRejected)
    {
        expectLimitBuyRejected(9995);
    }

    TEST_F(OrderBookTest, LimitPriceAboveMaxIsRejected)
    {
        expectLimitBuyRejected(20005);
    }

    TEST_F(OrderBookTest, LimitPriceOffTickIsRejected)
    {
        expectLimitBuyRejected(10001);
    }

    TEST_F(OrderBookTest, ModifyToPriceBelowMinIsRejected)
    {
        expectModifyRejected(9995);
    }

    TEST_F(OrderBookTest, ModifyToPriceAboveMaxIsRejected)
    {
        expectModifyRejected(20005);
    }

    TEST_F(OrderBookTest, ModifyToPriceOffTickIsRejected)
    {
        expectModifyRejected(15001);
    }

    TEST_F(OrderBookTest, ModifyToMinAndMaxPricesRests)
    {
        addLimit(1, types::Verb::Buy, 15000, 10);
        addLimit(2, types::Verb::Sell, 15005, 10);
        events.clear();

        modify(1, 10000, 10);
        modify(2, 20000, 10);

        EXPECT_TRUE(events.empty());
        EXPECT_EQ(book.bestBid(), 10000u);
        EXPECT_EQ(book.bestAsk(), 20000u);
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

    TEST_F(OrderBookTest, ModifiedPriceThatDoesNotCrossRestsWithoutEvents)
    {
        addLimit(1, types::Verb::Buy, 10000, 10);
        addLimit(2, types::Verb::Sell, 10200, 10);
        events.clear();

        modify(1, 10100, 10);

        EXPECT_TRUE(events.empty());
        EXPECT_EQ(book.bestBid(), 10100u);
        EXPECT_EQ(book.bestAsk(), 10200u);

        // The bid rests at its new price with its full quantity.
        addLimit(3, types::Verb::Sell, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::Fill);
        expectFill(events[0], 3, 1, 10100, 10, 0);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifyQtyUpAtSamePriceLosesTimePriority)
    {
        addLimit(1, types::Verb::Sell, 10100, 10);
        addLimit(2, types::Verb::Sell, 10100, 10);
        events.clear();

        modify(1, 10100, 15);

        EXPECT_TRUE(events.empty());

        // Order 2 is now first in the queue, and order 1 rests behind it with 15.
        addLimit(3, types::Verb::Buy, 10100, 25);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 3, 2, 10100, 10, 15);
        expectFill(events[1], 3, 1, 10100, 15, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifiedBidThatCrossesFillsPartlyThenRests)
    {
        addLimit(1, types::Verb::Sell, 10100, 4);
        addLimit(2, types::Verb::Buy, 10000, 10);
        events.clear();

        modify(2, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::Fill);
        expectFill(events[0], 2, 1, 10100, 4, 6);
        EXPECT_EQ(book.bestBid(), 10100u);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);

        // The rest of the bid rests at 10100.
        events.clear();
        addLimit(3, types::Verb::Sell, 10100, 6);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 3, 2, 10100, 6, 0);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifiedAskThatCrossesFillsPartlyThenRests)
    {
        addLimit(1, types::Verb::Buy, 10100, 4);
        addLimit(2, types::Verb::Sell, 10200, 10);
        events.clear();

        modify(2, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::Fill);
        expectFill(events[0], 2, 1, 10100, 4, 6);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), 10100u);

        // The rest of the ask rests at 10100.
        events.clear();
        addLimit(3, types::Verb::Buy, 10100, 6);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 3, 2, 10100, 6, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifiedBidThatCrossesFillsFully)
    {
        const uint32_t freeCount = book.poolFreeCount();
        addLimit(1, types::Verb::Sell, 10100, 10);
        addLimit(2, types::Verb::Buy, 10000, 10);
        events.clear();

        modify(2, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::Fill);
        expectFill(events[0], 2, 1, 10100, 10, 0);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);

        // The bid is gone, so cancelling it is rejected.
        events.clear();
        book.cancelOrder({.orderId = 2, .clientId = CLIENT});

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderRejected);
    }

    TEST_F(OrderBookTest, ModifiedAskThatCrossesFillsFully)
    {
        const uint32_t freeCount = book.poolFreeCount();
        addLimit(1, types::Verb::Buy, 10100, 10);
        addLimit(2, types::Verb::Sell, 10200, 10);
        events.clear();

        modify(2, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::Fill);
        expectFill(events[0], 2, 1, 10100, 10, 0);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);

        // The ask is gone, so cancelling it is rejected.
        events.clear();
        book.cancelOrder({.orderId = 2, .clientId = CLIENT});

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderRejected);
    }

    TEST_F(OrderBookTest, ModifiedBidThatCrossesSweepsLevels)
    {
        addLimit(1, types::Verb::Sell, 10100, 5);
        addLimit(2, types::Verb::Sell, 10105, 5);
        addLimit(3, types::Verb::Sell, 10110, 5);
        addLimit(4, types::Verb::Buy, 10000, 12);
        events.clear();

        modify(4, 10105, 12);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 4, 1, 10100, 5, 7);
        expectFill(events[1], 4, 2, 10105, 5, 2);
        EXPECT_EQ(book.bestBid(), 10105u);
        EXPECT_EQ(book.bestAsk(), 10110u);
    }

    TEST_F(OrderBookTest, ModifiedAskThatCrossesSweepsLevels)
    {
        addLimit(1, types::Verb::Buy, 10110, 5);
        addLimit(2, types::Verb::Buy, 10105, 5);
        addLimit(3, types::Verb::Buy, 10100, 5);
        addLimit(4, types::Verb::Sell, 10200, 12);
        events.clear();

        modify(4, 10105, 12);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 4, 1, 10110, 5, 7);
        expectFill(events[1], 4, 2, 10105, 5, 2);
        EXPECT_EQ(book.bestBid(), 10100u);
        EXPECT_EQ(book.bestAsk(), 10105u);
    }

    // Bitmap word boundaries. Level index i is price 10000 + 5i, and each word holds 64 levels:
    // 10315 is bit 63 of word 0, 10320 is bit 0 of word 1 and 10640 is bit 0 of word 2.

    TEST_F(OrderBookTest, BestBidMovesToPreviousWordWhenBestLevelEmpties)
    {
        addLimit(1, types::Verb::Buy, 10320, 10);
        addLimit(2, types::Verb::Buy, 10315, 10);
        EXPECT_EQ(book.bestBid(), 10320u);

        book.cancelOrder({.orderId = 1, .clientId = CLIENT});

        EXPECT_EQ(book.bestBid(), 10315u);
    }

    TEST_F(OrderBookTest, BestAskMovesToNextWordWhenBestLevelEmpties)
    {
        addLimit(1, types::Verb::Sell, 10315, 10);
        addLimit(2, types::Verb::Sell, 10320, 10);
        EXPECT_EQ(book.bestAsk(), 10315u);

        book.cancelOrder({.orderId = 1, .clientId = CLIENT});

        EXPECT_EQ(book.bestAsk(), 10320u);
    }

    TEST_F(OrderBookTest, BestBidSkipsEmptyWordsWhenBestLevelEmpties)
    {
        addLimit(1, types::Verb::Buy, 10640, 10);
        addLimit(2, types::Verb::Buy, 10000, 10);
        EXPECT_EQ(book.bestBid(), 10640u);

        book.cancelOrder({.orderId = 1, .clientId = CLIENT});

        EXPECT_EQ(book.bestBid(), 10000u);
    }

    TEST_F(OrderBookTest, BestAskSkipsEmptyWordsWhenBestLevelEmpties)
    {
        addLimit(1, types::Verb::Sell, 10000, 10);
        addLimit(2, types::Verb::Sell, 10640, 10);
        EXPECT_EQ(book.bestAsk(), 10000u);

        book.cancelOrder({.orderId = 1, .clientId = CLIENT});

        EXPECT_EQ(book.bestAsk(), 10640u);
    }

    TEST_F(OrderBookTest, BestPricesClearWhenLevelsAtRangeEdgesEmpty)
    {
        addLimit(1, types::Verb::Buy, 10000, 10);
        addLimit(2, types::Verb::Sell, 20000, 10);
        EXPECT_EQ(book.bestBid(), 10000u);
        EXPECT_EQ(book.bestAsk(), 20000u);

        book.cancelOrder({.orderId = 1, .clientId = CLIENT});
        book.cancelOrder({.orderId = 2, .clientId = CLIENT});

        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    // The FOK check walks levels with nextBid()/nextAsk(), so each level must be counted once.

    TEST_F(OrderBookTest, FokSellWalksBidLevelsAcrossWordBoundary)
    {
        addLimit(1, types::Verb::Buy, 10320, 5);
        addLimit(2, types::Verb::Buy, 10315, 5);
        events.clear();

        book.addOrder(
            {.orderId = 3,
             .clientId = CLIENT,
             .side = types::Verb::Sell,
             .orderType = types::OrderType::Limit,
             .tif = types::TimeInForce::FOK,
             .limitPrice = 10315,
             .qty = 11}
        );

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderRejected);
        EXPECT_EQ(book.bestBid(), 10320u);
    }

    TEST_F(OrderBookTest, FokBuyWalksAskLevelsAcrossWordBoundary)
    {
        addLimit(1, types::Verb::Sell, 10315, 5);
        addLimit(2, types::Verb::Sell, 10320, 5);
        events.clear();

        book.addOrder(
            {.orderId = 3,
             .clientId = CLIENT,
             .side = types::Verb::Buy,
             .orderType = types::OrderType::Limit,
             .tif = types::TimeInForce::FOK,
             .limitPrice = 10320,
             .qty = 11}
        );

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderRejected);
        EXPECT_EQ(book.bestAsk(), 10315u);
    }

    // The walk asks for the level below minPrice / above maxPrice, and must find none.

    TEST_F(OrderBookTest, FokSellStopsWalkingAtMinPrice)
    {
        addLimit(1, types::Verb::Buy, 10000, 5);
        events.clear();

        book.addOrder(
            {.orderId = 2,
             .clientId = CLIENT,
             .side = types::Verb::Sell,
             .orderType = types::OrderType::Limit,
             .tif = types::TimeInForce::FOK,
             .limitPrice = 10000,
             .qty = 6}
        );

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderRejected);
        EXPECT_EQ(book.bestBid(), 10000u);
    }

    TEST_F(OrderBookTest, FokBuyStopsWalkingAtMaxPrice)
    {
        addLimit(1, types::Verb::Sell, 20000, 5);
        events.clear();

        book.addOrder(
            {.orderId = 2,
             .clientId = CLIENT,
             .side = types::Verb::Buy,
             .orderType = types::OrderType::Limit,
             .tif = types::TimeInForce::FOK,
             .limitPrice = 20000,
             .qty = 6}
        );

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderRejected);
        EXPECT_EQ(book.bestAsk(), 20000u);
    }

    // remove() and reduce() find a resting order's level from the level handle in its slot, and
    // debug builds assert that the handle matches the order's price. These tests reach orders
    // that are reduced, behind the head of their queue, away from the best price, moved, or in a
    // reused slot.

    TEST_F(OrderBookTest, PartlyFilledRestingOrderKeepsItsRemainder)
    {
        addLimit(1, types::Verb::Sell, 10100, 10);
        events.clear();

        addLimit(2, types::Verb::Buy, 10100, 4);

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::PartialFill);
        expectFill(events[0], 2, 1, 10100, 4, 0);
        EXPECT_EQ(events[0].passiveRemaining, 6u);
        EXPECT_EQ(book.bestAsk(), 10100u);

        // The rest of the sell still rests at 10100.
        events.clear();
        addLimit(3, types::Verb::Buy, 10100, 6);

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::Fill);
        expectFill(events[0], 3, 1, 10100, 6, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifyQtyDownAtSamePriceKeepsTimePriority)
    {
        addLimit(1, types::Verb::Sell, 10100, 10);
        addLimit(2, types::Verb::Sell, 10100, 10);
        events.clear();

        modify(1, 10100, 4);

        EXPECT_TRUE(events.empty());

        // Order 1 is still first in the queue, now with 4.
        addLimit(3, types::Verb::Buy, 10100, 14);

        ASSERT_EQ(events.size(), 2u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::Fill);
        expectFill(events[0], 3, 1, 10100, 4, 10);
        EXPECT_EQ(events[1].type, TradeEvent::Type::Fill);
        expectFill(events[1], 3, 2, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, CancelInMiddleOfQueueKeepsOtherOrdersInPlace)
    {
        addLimit(1, types::Verb::Sell, 10100, 10);
        addLimit(2, types::Verb::Sell, 10100, 10);
        addLimit(3, types::Verb::Sell, 10100, 10);
        events.clear();

        book.cancelOrder({.orderId = 2, .clientId = CLIENT});

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderCancelled);
        EXPECT_EQ(book.bestAsk(), 10100u);

        // Orders 1 and 3 fill in their original order.
        events.clear();
        addLimit(4, types::Verb::Buy, 10100, 20);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 4, 1, 10100, 10, 10);
        expectFill(events[1], 4, 3, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, CancelAtTailOfQueueKeepsOtherOrdersInPlace)
    {
        addLimit(1, types::Verb::Sell, 10100, 10);
        addLimit(2, types::Verb::Sell, 10100, 10);
        addLimit(3, types::Verb::Sell, 10100, 10);
        events.clear();

        book.cancelOrder({.orderId = 3, .clientId = CLIENT});

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderCancelled);

        // A new order joins the queue behind order 2, the new tail.
        addLimit(4, types::Verb::Sell, 10100, 10);
        events.clear();
        addLimit(5, types::Verb::Buy, 10100, 30);

        ASSERT_EQ(events.size(), 3u);
        expectFill(events[0], 5, 1, 10100, 10, 20);
        expectFill(events[1], 5, 2, 10100, 10, 10);
        expectFill(events[2], 5, 4, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, CancelLastBidAtLevelBelowBestKeepsBestBid)
    {
        addLimit(1, types::Verb::Buy, 10100, 10);
        addLimit(2, types::Verb::Buy, 10050, 10);
        events.clear();

        book.cancelOrder({.orderId = 2, .clientId = CLIENT});

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderCancelled);
        EXPECT_EQ(book.bestBid(), 10100u);

        // Only order 1 is left to fill, and the rest of the sell rests.
        events.clear();
        addLimit(3, types::Verb::Sell, 10000, 20);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 3, 1, 10100, 10, 10);
        EXPECT_EQ(events[1].type, TradeEvent::Type::OrderAccepted);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), 10000u);
    }

    TEST_F(OrderBookTest, CancelLastAskAtLevelAboveBestKeepsBestAsk)
    {
        addLimit(1, types::Verb::Sell, 10100, 10);
        addLimit(2, types::Verb::Sell, 10150, 10);
        events.clear();

        book.cancelOrder({.orderId = 2, .clientId = CLIENT});

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderCancelled);
        EXPECT_EQ(book.bestAsk(), 10100u);

        // Only order 1 is left to fill, and the rest of the buy rests.
        events.clear();
        addLimit(3, types::Verb::Buy, 10200, 20);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 3, 1, 10100, 10, 10);
        EXPECT_EQ(events[1].type, TradeEvent::Type::OrderAccepted);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.bestBid(), 10200u);
    }

    TEST_F(OrderBookTest, PartlyFilledOrderCanBeCancelled)
    {
        const uint32_t freeCount = book.poolFreeCount();
        addLimit(1, types::Verb::Sell, 10100, 10);
        addLimit(2, types::Verb::Buy, 10100, 4);
        events.clear();

        book.cancelOrder({.orderId = 1, .clientId = CLIENT});

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderCancelled);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }

    TEST_F(OrderBookTest, MovedOrderIsCancelledFromItsNewLevel)
    {
        addLimit(1, types::Verb::Buy, 10000, 10);
        modify(1, 10100, 10);
        EXPECT_EQ(book.bestBid(), 10100u);
        events.clear();

        book.cancelOrder({.orderId = 1, .clientId = CLIENT});

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderCancelled);
        EXPECT_EQ(book.bestBid(), NO_PRICE);

        // Nothing is left at the old price, so a sell there rests.
        events.clear();
        addLimit(2, types::Verb::Sell, 10000, 10);

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderAccepted);
        EXPECT_EQ(book.bestAsk(), 10000u);
    }

    TEST_F(OrderBookTest, ReusedSlotIsCancelledFromItsNewLevel)
    {
        const uint32_t freeCount = book.poolFreeCount();
        addLimit(1, types::Verb::Buy, 10100, 10);
        book.cancelOrder({.orderId = 1, .clientId = CLIENT});

        // The pool hands the slot that order 1 freed to order 2.
        addLimit(2, types::Verb::Buy, 10200, 10);
        EXPECT_EQ(book.bestBid(), 10200u);
        events.clear();

        book.cancelOrder({.orderId = 2, .clientId = CLIENT});

        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].type, TradeEvent::Type::OrderCancelled);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }
} // namespace
