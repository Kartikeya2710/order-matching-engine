#include <gtest/gtest.h>

#include <vector>

#include "OrderBook.hpp"

using namespace engine;
using namespace engine::book;
using engine::core::TradeEvent;

namespace
{
    using enum types::Verb;
    using enum types::OrderType;
    using enum types::TimeInForce;
    using enum types::SelfTradePrevention;
    using enum TradeEvent::Type;

    // Tests that are not about self-trade prevention send buys from ALICE and sells from BOB.
    constexpr types::ClientId ALICE = 7;
    constexpr types::ClientId BOB = 8;

    struct OrderOptions
    {
        types::TimeInForce tif = GTC;
        types::SelfTradePrevention stp = CancelResting;
    };

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

        void add(
            types::ClientId client,
            types::OrderId id,
            types::Verb side,
            types::OrderType orderType,
            types::Price price,
            types::Quantity qty,
            OrderOptions opts
        )
        {
            book.addOrder(
                {.orderId = id,
                 .clientId = client,
                 .side = side,
                 .orderType = orderType,
                 .tif = opts.tif,
                 .stp = opts.stp,
                 .limitPrice = price,
                 .qty = qty}
            );
        }

        void buy(
            types::ClientId client,
            types::OrderId id,
            types::Price price,
            types::Quantity qty,
            OrderOptions opts = {}
        )
        {
            add(client, id, Buy, Limit, price, qty, opts);
        }

        void sell(
            types::ClientId client,
            types::OrderId id,
            types::Price price,
            types::Quantity qty,
            OrderOptions opts = {}
        )
        {
            add(client, id, Sell, Limit, price, qty, opts);
        }

        void marketBuy(
            types::ClientId client,
            types::OrderId id,
            types::Quantity qty,
            types::SelfTradePrevention stp = CancelResting
        )
        {
            add(client, id, Buy, Market, NO_PRICE, qty, {.tif = IOC, .stp = stp});
        }

        void marketSell(types::ClientId client, types::OrderId id, types::Quantity qty)
        {
            add(client, id, Sell, Market, NO_PRICE, qty, {.tif = IOC});
        }

        void cancel(types::ClientId client, types::OrderId id)
        {
            book.cancelOrder({.orderId = id, .clientId = client});
        }

        void modify(
            types::ClientId client,
            types::OrderId id,
            types::Price newPrice,
            types::Quantity newQty
        )
        {
            book.modifyOrder(
                {.orderId = id, .clientId = client, .newLimitPrice = newPrice, .newQty = newQty}
            );
        }

        // The passive order is filled in full.
        static void expectFill(
            const TradeEvent& ev,
            types::OrderId aggressorId,
            types::OrderId passiveId,
            types::Price price,
            types::Quantity qty,
            types::Quantity aggressorRemaining
        )
        {
            EXPECT_EQ(ev.type, Fill);
            EXPECT_EQ(ev.aggressorOrderId, aggressorId);
            EXPECT_EQ(ev.passiveOrderId, passiveId);
            EXPECT_EQ(ev.fillPrice, price);
            EXPECT_EQ(ev.fillQty, qty);
            EXPECT_EQ(ev.aggressorRemaining, aggressorRemaining);
            EXPECT_EQ(ev.passiveRemaining, 0u);
        }

        // The aggressor is filled in full, and the passive order keeps passiveRemaining.
        static void expectPartialFill(
            const TradeEvent& ev,
            types::OrderId aggressorId,
            types::OrderId passiveId,
            types::Price price,
            types::Quantity qty,
            types::Quantity passiveRemaining
        )
        {
            EXPECT_EQ(ev.type, PartialFill);
            EXPECT_EQ(ev.aggressorOrderId, aggressorId);
            EXPECT_EQ(ev.passiveOrderId, passiveId);
            EXPECT_EQ(ev.fillPrice, price);
            EXPECT_EQ(ev.fillQty, qty);
            EXPECT_EQ(ev.aggressorRemaining, 0u);
            EXPECT_EQ(ev.passiveRemaining, passiveRemaining);
        }

        static void expectAccepted(
            const TradeEvent& ev,
            types::OrderId orderId,
            types::Quantity qty
        )
        {
            EXPECT_EQ(ev.type, OrderAccepted);
            EXPECT_EQ(ev.aggressorOrderId, orderId);
            EXPECT_EQ(ev.aggressorRemaining, qty);
        }

        static void expectRejected(const TradeEvent& ev, types::OrderId orderId)
        {
            EXPECT_EQ(ev.type, OrderRejected);
            EXPECT_EQ(ev.aggressorOrderId, orderId);
        }

        static void expectCancelled(const TradeEvent& ev, types::OrderId orderId)
        {
            EXPECT_EQ(ev.type, OrderCancelled);
            EXPECT_EQ(ev.aggressorOrderId, orderId);
        }

        // ALICE's order cancelledId is cancelled with cancelledQty open. cancelledBy is the
        // incoming order that cancelled a resting order, or 0 when the incoming order itself is
        // cancelled.
        static void expectSelfTradeCancelled(
            const TradeEvent& ev,
            types::OrderId cancelledId,
            types::Quantity cancelledQty,
            types::OrderId cancelledBy = 0
        )
        {
            EXPECT_EQ(ev.type, SelfTradeCancelled);
            EXPECT_EQ(ev.aggressorOrderId, cancelledId);
            EXPECT_EQ(ev.aggressorClientId, ALICE);
            EXPECT_EQ(ev.aggressorRemaining, cancelledQty);
            EXPECT_EQ(ev.passiveOrderId, cancelledBy);
        }

        // Sends a 0-quantity buy against a resting sell and checks that it is rejected
        // without touching the book.
        void expectZeroQtyBuyRejected(
            types::OrderType orderType,
            types::TimeInForce tif,
            types::Price limitPrice
        )
        {
            sell(BOB, 1, 10100, 10);
            const uint32_t freeCount = book.poolFreeCount();
            events.clear();

            add(ALICE, 2, Buy, orderType, limitPrice, 0, {.tif = tif});

            ASSERT_EQ(events.size(), 1u);
            expectRejected(events[0], 2);
            EXPECT_EQ(book.bestBid(), NO_PRICE);
            EXPECT_EQ(book.bestAsk(), 10100u);
            EXPECT_EQ(book.poolFreeCount(), freeCount);

            // The resting sell still has its full quantity.
            events.clear();
            buy(ALICE, 3, 10100, 10);

            ASSERT_EQ(events.size(), 1u);
            expectFill(events[0], 3, 1, 10100, 10, 0);
            EXPECT_EQ(book.bestAsk(), NO_PRICE);
        }

        // Sends a limit buy at `price` against a resting sell at 15000 and checks that it is
        // rejected without touching the book.
        void expectLimitBuyRejected(types::Price price)
        {
            sell(BOB, 1, 15000, 10);
            const uint32_t freeCount = book.poolFreeCount();
            events.clear();

            buy(ALICE, 2, price, 10);

            ASSERT_EQ(events.size(), 1u);
            expectRejected(events[0], 2);
            EXPECT_EQ(book.bestBid(), NO_PRICE);
            EXPECT_EQ(book.bestAsk(), 15000u);
            EXPECT_EQ(book.poolFreeCount(), freeCount);
        }

        // Modifies a resting buy at 15000 to `price` and checks that the modify is rejected and
        // the buy still rests at 15000 with its full quantity.
        void expectModifyRejected(types::Price price)
        {
            buy(ALICE, 1, 15000, 10);
            events.clear();

            modify(ALICE, 1, price, 10);

            ASSERT_EQ(events.size(), 1u);
            expectRejected(events[0], 1);
            EXPECT_EQ(book.bestBid(), 15000u);

            events.clear();
            sell(BOB, 2, 15000, 10);

            ASSERT_EQ(events.size(), 1u);
            expectFill(events[0], 2, 1, 15000, 10, 0);
        }
    };

    TEST_F(OrderBookTest, ZeroQtyMarketOrderIsRejected)
    {
        expectZeroQtyBuyRejected(Market, IOC, NO_PRICE);
    }

    TEST_F(OrderBookTest, ZeroQtyLimitGtcOrderIsRejected)
    {
        expectZeroQtyBuyRejected(Limit, GTC, 10100);
    }

    TEST_F(OrderBookTest, ZeroQtyLimitIocOrderIsRejected)
    {
        expectZeroQtyBuyRejected(Limit, IOC, 10100);
    }

    TEST_F(OrderBookTest, ZeroQtyLimitFokOrderIsRejected)
    {
        expectZeroQtyBuyRejected(Limit, FOK, 10100);
    }

    TEST_F(OrderBookTest, ModifyToZeroQtyAtSamePriceIsRejected)
    {
        sell(BOB, 1, 10100, 10);
        events.clear();

        modify(BOB, 1, 10100, 0);

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 1);

        // The order still rests with its full quantity.
        events.clear();
        buy(ALICE, 2, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 2, 1, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifyToZeroQtyAtNewPriceIsRejected)
    {
        sell(BOB, 1, 10100, 10);
        events.clear();

        modify(BOB, 1, 10200, 0);

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 1);
        EXPECT_EQ(book.bestAsk(), 10100u);

        // The order still rests at its old price with its full quantity.
        events.clear();
        buy(ALICE, 2, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 2, 1, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifyOfUnknownOrderIsRejected)
    {
        buy(ALICE, 1, 10100, 10);
        events.clear();

        modify(ALICE, 2, 10105, 10);

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 2);
        EXPECT_EQ(book.bestBid(), 10100u);
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
        buy(ALICE, 1, 15000, 10);
        sell(BOB, 2, 15005, 10);
        events.clear();

        modify(ALICE, 1, 10000, 10);
        modify(BOB, 2, 20000, 10);

        EXPECT_TRUE(events.empty());
        EXPECT_EQ(book.bestBid(), 10000u);
        EXPECT_EQ(book.bestAsk(), 20000u);
    }

    TEST_F(OrderBookTest, ModifyWithSamePriceAndQtyKeepsTimePriority)
    {
        sell(BOB, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        events.clear();

        modify(BOB, 1, 10100, 10);

        EXPECT_TRUE(events.empty());

        // Order 1 is still first in the queue.
        buy(ALICE, 3, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 3, 1, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), 10100u);
    }

    TEST_F(OrderBookTest, ModifiedPriceThatDoesNotCrossRestsWithoutEvents)
    {
        buy(ALICE, 1, 10000, 10);
        sell(BOB, 2, 10200, 10);
        events.clear();

        modify(ALICE, 1, 10100, 10);

        EXPECT_TRUE(events.empty());
        EXPECT_EQ(book.bestBid(), 10100u);
        EXPECT_EQ(book.bestAsk(), 10200u);

        // The bid rests at its new price with its full quantity.
        sell(BOB, 3, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 3, 1, 10100, 10, 0);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifyQtyUpAtSamePriceLosesTimePriority)
    {
        sell(BOB, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        events.clear();

        modify(BOB, 1, 10100, 15);

        EXPECT_TRUE(events.empty());

        // Order 2 is now first in the queue, and order 1 rests behind it with 15.
        buy(ALICE, 3, 10100, 25);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 3, 2, 10100, 10, 15);
        expectFill(events[1], 3, 1, 10100, 15, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifiedBidThatCrossesFillsPartlyThenRests)
    {
        sell(BOB, 1, 10100, 4);
        buy(ALICE, 2, 10000, 10);
        events.clear();

        modify(ALICE, 2, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 2, 1, 10100, 4, 6);
        EXPECT_EQ(book.bestBid(), 10100u);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);

        // The rest of the bid rests at 10100.
        events.clear();
        sell(BOB, 3, 10100, 6);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 3, 2, 10100, 6, 0);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifiedAskThatCrossesFillsPartlyThenRests)
    {
        buy(ALICE, 1, 10100, 4);
        sell(BOB, 2, 10200, 10);
        events.clear();

        modify(BOB, 2, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 2, 1, 10100, 4, 6);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), 10100u);

        // The rest of the ask rests at 10100.
        events.clear();
        buy(ALICE, 3, 10100, 6);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 3, 2, 10100, 6, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifiedBidThatCrossesFillsFully)
    {
        const uint32_t freeCount = book.poolFreeCount();
        sell(BOB, 1, 10100, 10);
        buy(ALICE, 2, 10000, 10);
        events.clear();

        modify(ALICE, 2, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 2, 1, 10100, 10, 0);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);

        // The bid is gone, so cancelling it is rejected.
        events.clear();
        cancel(ALICE, 2);

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 2);
    }

    TEST_F(OrderBookTest, ModifiedAskThatCrossesFillsFully)
    {
        const uint32_t freeCount = book.poolFreeCount();
        buy(ALICE, 1, 10100, 10);
        sell(BOB, 2, 10200, 10);
        events.clear();

        modify(BOB, 2, 10100, 10);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 2, 1, 10100, 10, 0);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);

        // The ask is gone, so cancelling it is rejected.
        events.clear();
        cancel(BOB, 2);

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 2);
    }

    TEST_F(OrderBookTest, ModifiedBidThatCrossesSweepsLevels)
    {
        sell(BOB, 1, 10100, 5);
        sell(BOB, 2, 10105, 5);
        sell(BOB, 3, 10110, 5);
        buy(ALICE, 4, 10000, 12);
        events.clear();

        modify(ALICE, 4, 10105, 12);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 4, 1, 10100, 5, 7);
        expectFill(events[1], 4, 2, 10105, 5, 2);
        EXPECT_EQ(book.bestBid(), 10105u);
        EXPECT_EQ(book.bestAsk(), 10110u);
    }

    TEST_F(OrderBookTest, ModifiedAskThatCrossesSweepsLevels)
    {
        buy(ALICE, 1, 10110, 5);
        buy(ALICE, 2, 10105, 5);
        buy(ALICE, 3, 10100, 5);
        sell(BOB, 4, 10200, 12);
        events.clear();

        modify(BOB, 4, 10105, 12);

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
        buy(ALICE, 1, 10320, 10);
        buy(ALICE, 2, 10315, 10);
        EXPECT_EQ(book.bestBid(), 10320u);

        cancel(ALICE, 1);

        EXPECT_EQ(book.bestBid(), 10315u);
    }

    TEST_F(OrderBookTest, BestAskMovesToNextWordWhenBestLevelEmpties)
    {
        sell(BOB, 1, 10315, 10);
        sell(BOB, 2, 10320, 10);
        EXPECT_EQ(book.bestAsk(), 10315u);

        cancel(BOB, 1);

        EXPECT_EQ(book.bestAsk(), 10320u);
    }

    TEST_F(OrderBookTest, BestBidSkipsEmptyWordsWhenBestLevelEmpties)
    {
        buy(ALICE, 1, 10640, 10);
        buy(ALICE, 2, 10000, 10);
        EXPECT_EQ(book.bestBid(), 10640u);

        cancel(ALICE, 1);

        EXPECT_EQ(book.bestBid(), 10000u);
    }

    TEST_F(OrderBookTest, BestAskSkipsEmptyWordsWhenBestLevelEmpties)
    {
        sell(BOB, 1, 10000, 10);
        sell(BOB, 2, 10640, 10);
        EXPECT_EQ(book.bestAsk(), 10000u);

        cancel(BOB, 1);

        EXPECT_EQ(book.bestAsk(), 10640u);
    }

    TEST_F(OrderBookTest, BestPricesClearWhenLevelsAtRangeEdgesEmpty)
    {
        buy(ALICE, 1, 10000, 10);
        sell(BOB, 2, 20000, 10);
        EXPECT_EQ(book.bestBid(), 10000u);
        EXPECT_EQ(book.bestAsk(), 20000u);

        cancel(ALICE, 1);
        cancel(BOB, 2);

        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    // The FOK check walks levels with nextBid()/nextAsk(), so each level must be counted once.

    TEST_F(OrderBookTest, FokSellWalksBidLevelsAcrossWordBoundary)
    {
        buy(ALICE, 1, 10320, 5);
        buy(ALICE, 2, 10315, 5);
        events.clear();

        sell(BOB, 3, 10315, 11, {.tif = FOK});

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 3);
        EXPECT_EQ(book.bestBid(), 10320u);
    }

    TEST_F(OrderBookTest, FokBuyWalksAskLevelsAcrossWordBoundary)
    {
        sell(BOB, 1, 10315, 5);
        sell(BOB, 2, 10320, 5);
        events.clear();

        buy(ALICE, 3, 10320, 11, {.tif = FOK});

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 3);
        EXPECT_EQ(book.bestAsk(), 10315u);
    }

    // The walk asks for the level below minPrice / above maxPrice, and must find none.

    TEST_F(OrderBookTest, FokSellStopsWalkingAtMinPrice)
    {
        buy(ALICE, 1, 10000, 5);
        events.clear();

        sell(BOB, 2, 10000, 6, {.tif = FOK});

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 2);
        EXPECT_EQ(book.bestBid(), 10000u);
    }

    TEST_F(OrderBookTest, FokBuyStopsWalkingAtMaxPrice)
    {
        sell(BOB, 1, 20000, 5);
        events.clear();

        buy(ALICE, 2, 20000, 6, {.tif = FOK});

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 2);
        EXPECT_EQ(book.bestAsk(), 20000u);
    }

    // The walk counts only the levels within the FOK's limit.

    TEST_F(OrderBookTest, FokBuyIsKilledWhenItNeedsLevelsBeyondItsLimit)
    {
        sell(BOB, 1, 10100, 5);
        sell(BOB, 2, 10105, 10);
        const uint32_t freeCount = book.poolFreeCount();
        events.clear();

        buy(ALICE, 3, 10100, 10, {.tif = FOK});

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 3);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), 10100u);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }

    TEST_F(OrderBookTest, FokSellIsKilledWhenItNeedsLevelsBeyondItsLimit)
    {
        buy(ALICE, 1, 10105, 5);
        buy(ALICE, 2, 10100, 10);
        const uint32_t freeCount = book.poolFreeCount();
        events.clear();

        sell(BOB, 3, 10105, 10, {.tif = FOK});

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 3);
        EXPECT_EQ(book.bestBid(), 10105u);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }

    TEST_F(OrderBookTest, FokBuyFillsWhenLevelsHoldExactlyItsQuantity)
    {
        sell(BOB, 1, 10100, 5);
        sell(BOB, 2, 10105, 5);
        events.clear();

        buy(ALICE, 3, 10105, 10, {.tif = FOK});

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 3, 1, 10100, 5, 5);
        expectFill(events[1], 3, 2, 10105, 5, 0);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    // Market orders have no price limit. What the book cannot fill is rejected.

    TEST_F(OrderBookTest, MarketBuyFillsAcrossLevels)
    {
        sell(BOB, 1, 10100, 5);
        sell(BOB, 2, 10200, 10);
        events.clear();

        marketBuy(ALICE, 3, 10);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 3, 1, 10100, 5, 5);
        expectPartialFill(events[1], 3, 2, 10200, 5, 5);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), 10200u);
    }

    TEST_F(OrderBookTest, MarketSellRejectsWhatTheBookCannotFill)
    {
        buy(ALICE, 1, 10100, 5);
        events.clear();

        marketSell(BOB, 2, 8);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 2, 1, 10100, 5, 3);
        expectRejected(events[1], 2);
        EXPECT_EQ(events[1].aggressorRemaining, 3u);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    // remove() and reduce() find a resting order's level from the level handle in its slot, and
    // debug builds assert that the handle matches the order's price. These tests reach orders
    // that are reduced, behind the head of their queue, away from the best price, moved, or in a
    // reused slot.

    TEST_F(OrderBookTest, PartlyFilledRestingOrderKeepsItsRemainder)
    {
        sell(BOB, 1, 10100, 10);
        events.clear();

        buy(ALICE, 2, 10100, 4);

        ASSERT_EQ(events.size(), 1u);
        expectPartialFill(events[0], 2, 1, 10100, 4, 6);
        EXPECT_EQ(book.bestAsk(), 10100u);

        // The rest of the sell still rests at 10100.
        events.clear();
        buy(ALICE, 3, 10100, 6);

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 3, 1, 10100, 6, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, ModifyQtyDownAtSamePriceKeepsTimePriority)
    {
        sell(BOB, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        events.clear();

        modify(BOB, 1, 10100, 4);

        EXPECT_TRUE(events.empty());

        // Order 1 is still first in the queue, now with 4.
        buy(ALICE, 3, 10100, 14);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 3, 1, 10100, 4, 10);
        expectFill(events[1], 3, 2, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, CancelInMiddleOfQueueKeepsOtherOrdersInPlace)
    {
        sell(BOB, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        sell(BOB, 3, 10100, 10);
        events.clear();

        cancel(BOB, 2);

        ASSERT_EQ(events.size(), 1u);
        expectCancelled(events[0], 2);
        EXPECT_EQ(book.bestAsk(), 10100u);

        // Orders 1 and 3 fill in their original order.
        events.clear();
        buy(ALICE, 4, 10100, 20);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 4, 1, 10100, 10, 10);
        expectFill(events[1], 4, 3, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, CancelAtTailOfQueueKeepsOtherOrdersInPlace)
    {
        sell(BOB, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        sell(BOB, 3, 10100, 10);
        events.clear();

        cancel(BOB, 3);

        ASSERT_EQ(events.size(), 1u);
        expectCancelled(events[0], 3);

        // A new order joins the queue behind order 2, the new tail.
        sell(BOB, 4, 10100, 10);
        events.clear();
        buy(ALICE, 5, 10100, 30);

        ASSERT_EQ(events.size(), 3u);
        expectFill(events[0], 5, 1, 10100, 10, 20);
        expectFill(events[1], 5, 2, 10100, 10, 10);
        expectFill(events[2], 5, 4, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, CancelLastBidAtLevelBelowBestKeepsBestBid)
    {
        buy(ALICE, 1, 10100, 10);
        buy(ALICE, 2, 10050, 10);
        events.clear();

        cancel(ALICE, 2);

        ASSERT_EQ(events.size(), 1u);
        expectCancelled(events[0], 2);
        EXPECT_EQ(book.bestBid(), 10100u);

        // Only order 1 is left to fill, and the rest of the sell rests.
        events.clear();
        sell(BOB, 3, 10000, 20);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 3, 1, 10100, 10, 10);
        expectAccepted(events[1], 3, 10);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), 10000u);
    }

    TEST_F(OrderBookTest, CancelLastAskAtLevelAboveBestKeepsBestAsk)
    {
        sell(BOB, 1, 10100, 10);
        sell(BOB, 2, 10150, 10);
        events.clear();

        cancel(BOB, 2);

        ASSERT_EQ(events.size(), 1u);
        expectCancelled(events[0], 2);
        EXPECT_EQ(book.bestAsk(), 10100u);

        // Only order 1 is left to fill, and the rest of the buy rests.
        events.clear();
        buy(ALICE, 3, 10200, 20);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 3, 1, 10100, 10, 10);
        expectAccepted(events[1], 3, 10);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.bestBid(), 10200u);
    }

    TEST_F(OrderBookTest, PartlyFilledOrderCanBeCancelled)
    {
        const uint32_t freeCount = book.poolFreeCount();
        sell(BOB, 1, 10100, 10);
        buy(ALICE, 2, 10100, 4);
        events.clear();

        cancel(BOB, 1);

        ASSERT_EQ(events.size(), 1u);
        expectCancelled(events[0], 1);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }

    TEST_F(OrderBookTest, MovedOrderIsCancelledFromItsNewLevel)
    {
        buy(ALICE, 1, 10000, 10);
        modify(ALICE, 1, 10100, 10);
        EXPECT_EQ(book.bestBid(), 10100u);
        events.clear();

        cancel(ALICE, 1);

        ASSERT_EQ(events.size(), 1u);
        expectCancelled(events[0], 1);
        EXPECT_EQ(book.bestBid(), NO_PRICE);

        // Nothing is left at the old price, so a sell there rests.
        events.clear();
        sell(BOB, 2, 10000, 10);

        ASSERT_EQ(events.size(), 1u);
        expectAccepted(events[0], 2, 10);
        EXPECT_EQ(book.bestAsk(), 10000u);
    }

    TEST_F(OrderBookTest, ReusedSlotIsCancelledFromItsNewLevel)
    {
        const uint32_t freeCount = book.poolFreeCount();
        buy(ALICE, 1, 10100, 10);
        cancel(ALICE, 1);

        // The pool hands the slot that order 1 freed to order 2.
        buy(ALICE, 2, 10200, 10);
        EXPECT_EQ(book.bestBid(), 10200u);
        events.clear();

        cancel(ALICE, 2);

        ASSERT_EQ(events.size(), 1u);
        expectCancelled(events[0], 2);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }

    TEST_F(OrderBookTest, BookMatchesWithoutEventCallback)
    {
        const uint32_t freeCount = book.poolFreeCount();
        book.setEventCallback(nullptr);

        sell(BOB, 1, 10100, 10);
        buy(ALICE, 2, 10100, 4);

        EXPECT_EQ(book.bestAsk(), 10100u);
        EXPECT_EQ(book.poolFreeCount(), freeCount - 1);

        // Only 6 of order 1 is left.
        buy(ALICE, 3, 10100, 6);

        EXPECT_TRUE(events.empty());
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }

    // Self-trade prevention. ALICE's incoming order meets resting orders from BOB and from ALICE.
    // Only the incoming order's mode counts.

    // An order that does not set a mode uses cancel resting.
    TEST_F(OrderBookTest, CancelRestingIsTheDefaultMode)
    {
        sell(ALICE, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        events.clear();

        // Sent without the helpers, which always set a mode.
        book.addOrder(
            {.orderId = 3,
             .clientId = ALICE,
             .side = Buy,
             .orderType = Limit,
             .tif = GTC,
             .limitPrice = 10100,
             .qty = 10}
        );

        ASSERT_EQ(events.size(), 2u);
        expectSelfTradeCancelled(events[0], 1, 10, 3);
        expectFill(events[1], 3, 2, 10100, 10, 0);
    }

    // Cancel resting: the incoming order cancels each own order it reaches and keeps matching.

    TEST_F(OrderBookTest, CancelRestingCancelsOwnOrderAtFrontOfLevel)
    {
        const uint32_t freeCount = book.poolFreeCount();
        sell(ALICE, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        events.clear();

        buy(ALICE, 3, 10100, 10, {.stp = CancelResting});

        ASSERT_EQ(events.size(), 2u);
        expectSelfTradeCancelled(events[0], 1, 10, 3);
        expectFill(events[1], 3, 2, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }

    TEST_F(OrderBookTest, CancelRestingCancelsOwnOrderInMiddleOfLevel)
    {
        sell(BOB, 1, 10100, 10);
        sell(ALICE, 2, 10100, 10);
        sell(BOB, 3, 10100, 10);
        events.clear();

        buy(ALICE, 4, 10100, 20, {.stp = CancelResting});

        ASSERT_EQ(events.size(), 3u);
        expectFill(events[0], 4, 1, 10100, 10, 10);
        expectSelfTradeCancelled(events[1], 2, 10, 4);
        expectFill(events[2], 4, 3, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, CancelRestingCancelsOwnOrderAtBackOfLevelThenRests)
    {
        sell(BOB, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        sell(ALICE, 3, 10100, 10);
        events.clear();

        buy(ALICE, 4, 10100, 25, {.stp = CancelResting});

        ASSERT_EQ(events.size(), 4u);
        expectFill(events[0], 4, 1, 10100, 10, 15);
        expectFill(events[1], 4, 2, 10100, 10, 5);
        expectSelfTradeCancelled(events[2], 3, 10, 4);
        expectAccepted(events[3], 4, 5);
        EXPECT_EQ(book.bestBid(), 10100u);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, CancelRestingLeavesOwnOrderItDoesNotReach)
    {
        sell(BOB, 1, 10100, 10);
        sell(ALICE, 2, 10100, 10);
        events.clear();

        buy(ALICE, 3, 10100, 10, {.stp = CancelResting});

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 3, 1, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), 10100u);
    }

    TEST_F(OrderBookTest, CancelRestingCancelsOwnOrdersAcrossLevels)
    {
        sell(ALICE, 1, 10100, 10);
        sell(BOB, 2, 10105, 10);
        sell(ALICE, 3, 10110, 10);
        sell(BOB, 4, 10110, 10);
        events.clear();

        buy(ALICE, 5, 10110, 20, {.stp = CancelResting});

        ASSERT_EQ(events.size(), 4u);
        expectSelfTradeCancelled(events[0], 1, 10, 5);
        expectFill(events[1], 5, 2, 10105, 10, 10);
        expectSelfTradeCancelled(events[2], 3, 10, 5);
        expectFill(events[3], 5, 4, 10110, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, CancelRestingCancelsWhatIsLeftOfPartlyFilledOwnOrder)
    {
        const uint32_t freeCount = book.poolFreeCount();
        sell(ALICE, 1, 10100, 10);
        // BOB fills 4 of order 1.
        buy(BOB, 2, 10100, 4);
        sell(BOB, 3, 10100, 10);
        events.clear();

        buy(ALICE, 4, 10100, 10, {.stp = CancelResting});

        ASSERT_EQ(events.size(), 2u);
        expectSelfTradeCancelled(events[0], 1, 6, 4);
        expectFill(events[1], 4, 3, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }

    // Cancel aggressor: the incoming order stops at the first own order it reaches, and what is
    // left of it is cancelled. The own order and the orders behind it keep resting.

    TEST_F(OrderBookTest, CancelAggressorAtFrontOfLevelCancelsWholeOrder)
    {
        sell(ALICE, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        const uint32_t freeCount = book.poolFreeCount();
        events.clear();

        buy(ALICE, 3, 10100, 10, {.stp = CancelAggressor});

        ASSERT_EQ(events.size(), 1u);
        expectSelfTradeCancelled(events[0], 3, 10);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), 10100u);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }

    TEST_F(OrderBookTest, CancelAggressorInMiddleOfLevelCancelsTheRest)
    {
        sell(BOB, 1, 10100, 10);
        sell(ALICE, 2, 10100, 10);
        sell(BOB, 3, 10100, 10);
        events.clear();

        buy(ALICE, 4, 10100, 25, {.stp = CancelAggressor});

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 4, 1, 10100, 10, 15);
        expectSelfTradeCancelled(events[1], 4, 15);
        EXPECT_EQ(book.bestBid(), NO_PRICE);

        // Orders 2 and 3 still rest.
        events.clear();
        cancel(ALICE, 2);
        cancel(BOB, 3);

        ASSERT_EQ(events.size(), 2u);
        expectCancelled(events[0], 2);
        expectCancelled(events[1], 3);
    }

    TEST_F(OrderBookTest, CancelAggressorAtBackOfLevelCancelsTheRest)
    {
        sell(BOB, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        sell(ALICE, 3, 10100, 10);
        events.clear();

        buy(ALICE, 4, 10100, 25, {.stp = CancelAggressor});

        ASSERT_EQ(events.size(), 3u);
        expectFill(events[0], 4, 1, 10100, 10, 15);
        expectFill(events[1], 4, 2, 10100, 10, 5);
        expectSelfTradeCancelled(events[2], 4, 5);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), 10100u);
    }

    TEST_F(OrderBookTest, CancelAggressorStopsAtOwnOrderOnLaterLevel)
    {
        sell(BOB, 1, 10100, 10);
        sell(ALICE, 2, 10105, 10);
        sell(BOB, 3, 10105, 10);
        events.clear();

        buy(ALICE, 4, 10105, 20, {.stp = CancelAggressor});

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 4, 1, 10100, 10, 10);
        expectSelfTradeCancelled(events[1], 4, 10);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), 10105u);
    }

    // Market and IOC buys. What other clients cannot fill is rejected (market) or discarded (IOC),
    // but a remainder cancelled by self-trade prevention is reported only as cancelled.

    TEST_F(OrderBookTest, MarketCancelRestingRejectsWhatOthersCannotFill)
    {
        sell(ALICE, 1, 10100, 10);
        sell(BOB, 2, 10105, 10);
        events.clear();

        marketBuy(ALICE, 3, 15, CancelResting);

        ASSERT_EQ(events.size(), 3u);
        expectSelfTradeCancelled(events[0], 1, 10, 3);
        expectFill(events[1], 3, 2, 10105, 10, 5);
        expectRejected(events[2], 3);
        EXPECT_EQ(events[2].aggressorRemaining, 5u);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, MarketCancelAggressorIsCancelledNotRejected)
    {
        sell(BOB, 1, 10100, 10);
        sell(ALICE, 2, 10105, 10);
        events.clear();

        marketBuy(ALICE, 3, 15, CancelAggressor);

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 3, 1, 10100, 10, 5);
        expectSelfTradeCancelled(events[1], 3, 5);
        EXPECT_EQ(book.bestAsk(), 10105u);
    }

    TEST_F(OrderBookTest, IocCancelRestingDiscardsWhatOthersCannotFill)
    {
        sell(ALICE, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        events.clear();

        buy(ALICE, 3, 10100, 15, {.tif = IOC, .stp = CancelResting});

        ASSERT_EQ(events.size(), 2u);
        expectSelfTradeCancelled(events[0], 1, 10, 3);
        expectFill(events[1], 3, 2, 10100, 10, 5);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, IocCancelAggressorIsCancelledAtOwnOrder)
    {
        sell(BOB, 1, 10100, 10);
        sell(ALICE, 2, 10100, 10);
        events.clear();

        buy(ALICE, 3, 10100, 15, {.tif = IOC, .stp = CancelAggressor});

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 3, 1, 10100, 10, 5);
        expectSelfTradeCancelled(events[1], 3, 5);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), 10100u);
    }

    // A FOK buy counts only what it can trade: under cancel resting, the sells of other clients;
    // under cancel aggressor, the sells ahead of the first own sell. A killed FOK touches nothing.

    TEST_F(OrderBookTest, FokCancelRestingFillsFromOtherClients)
    {
        sell(ALICE, 1, 10100, 10);
        sell(BOB, 2, 10100, 10);
        events.clear();

        buy(ALICE, 3, 10100, 10, {.tif = FOK, .stp = CancelResting});

        ASSERT_EQ(events.size(), 2u);
        expectSelfTradeCancelled(events[0], 1, 10, 3);
        expectFill(events[1], 3, 2, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    TEST_F(OrderBookTest, FokCancelRestingIsKilledWhenOwnOrdersAreNeeded)
    {
        sell(ALICE, 1, 10100, 10);
        sell(BOB, 2, 10100, 5);
        const uint32_t freeCount = book.poolFreeCount();
        events.clear();

        buy(ALICE, 3, 10100, 10, {.tif = FOK, .stp = CancelResting});

        // Order 1 is not cancelled, so no slot is freed.
        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 3);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
        EXPECT_EQ(book.bestAsk(), 10100u);
    }

    TEST_F(OrderBookTest, FokCancelAggressorFillsBeforeOwnOrder)
    {
        sell(BOB, 1, 10100, 10);
        sell(ALICE, 2, 10100, 10);
        events.clear();

        buy(ALICE, 3, 10100, 10, {.tif = FOK, .stp = CancelAggressor});

        ASSERT_EQ(events.size(), 1u);
        expectFill(events[0], 3, 1, 10100, 10, 0);
        EXPECT_EQ(book.bestAsk(), 10100u);
    }

    TEST_F(OrderBookTest, FokCancelAggressorIsKilledWhenOwnOrderComesFirst)
    {
        sell(BOB, 1, 10100, 5);
        sell(ALICE, 2, 10100, 10);
        sell(BOB, 3, 10100, 10);
        const uint32_t freeCount = book.poolFreeCount();
        events.clear();

        // BOB offers 15, but only 5 is ahead of order 2.
        buy(ALICE, 4, 10100, 10, {.tif = FOK, .stp = CancelAggressor});

        // Order 1 is not filled, so no slot is freed.
        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 4);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }

    // A modify that crosses matches with the mode the order entered with.

    TEST_F(OrderBookTest, ModifiedBidWithCancelAggressorIsCancelledAtOwnOrder)
    {
        const uint32_t freeCount = book.poolFreeCount();
        buy(ALICE, 1, 10000, 10, {.stp = CancelAggressor});
        sell(BOB, 2, 10100, 4);
        sell(ALICE, 3, 10100, 10);
        events.clear();

        modify(ALICE, 1, 10100, 10);

        // Only order 3 is left.
        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 1, 2, 10100, 4, 6);
        expectSelfTradeCancelled(events[1], 1, 6);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), 10100u);
        EXPECT_EQ(book.poolFreeCount(), freeCount - 1);

        // The bid is gone, so cancelling it is rejected.
        events.clear();
        cancel(ALICE, 1);

        ASSERT_EQ(events.size(), 1u);
        expectRejected(events[0], 1);
    }

    TEST_F(OrderBookTest, ModifiedBidWithCancelRestingCancelsOwnOrderThenRests)
    {
        buy(ALICE, 1, 10000, 10, {.stp = CancelResting});
        sell(ALICE, 2, 10100, 10);
        sell(BOB, 3, 10100, 4);
        events.clear();

        modify(ALICE, 1, 10100, 10);

        ASSERT_EQ(events.size(), 2u);
        expectSelfTradeCancelled(events[0], 2, 10, 1);
        expectFill(events[1], 1, 3, 10100, 4, 6);
        EXPECT_EQ(book.bestBid(), 10100u);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }

    // The same rules for an incoming sell.

    TEST_F(OrderBookTest, SellCancelRestingCancelsOwnBidsAcrossLevels)
    {
        const uint32_t freeCount = book.poolFreeCount();
        buy(ALICE, 1, 10110, 10);
        buy(BOB, 2, 10105, 10);
        buy(ALICE, 3, 10100, 10);
        buy(BOB, 4, 10100, 10);
        events.clear();

        sell(ALICE, 5, 10100, 20, {.stp = CancelResting});

        ASSERT_EQ(events.size(), 4u);
        expectSelfTradeCancelled(events[0], 1, 10, 5);
        expectFill(events[1], 5, 2, 10105, 10, 10);
        expectSelfTradeCancelled(events[2], 3, 10, 5);
        expectFill(events[3], 5, 4, 10100, 10, 0);
        EXPECT_EQ(book.bestBid(), NO_PRICE);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
        EXPECT_EQ(book.poolFreeCount(), freeCount);
    }

    TEST_F(OrderBookTest, SellCancelAggressorStopsAtOwnBidOnLaterLevel)
    {
        buy(BOB, 1, 10105, 10);
        buy(ALICE, 2, 10100, 10);
        buy(BOB, 3, 10100, 10);
        events.clear();

        sell(ALICE, 4, 10100, 20, {.stp = CancelAggressor});

        ASSERT_EQ(events.size(), 2u);
        expectFill(events[0], 4, 1, 10105, 10, 10);
        expectSelfTradeCancelled(events[1], 4, 10);
        EXPECT_EQ(book.bestBid(), 10100u);
        EXPECT_EQ(book.bestAsk(), NO_PRICE);
    }
} // namespace
