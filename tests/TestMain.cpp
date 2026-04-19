// End-to-end test suite for the Order Matching Engine.
// Tests are entirely synchronous — they call OrderBook directly without
// threading, which makes assertions deterministic and debuggable.
// Integration tests that exercise MatchingCore + coroutines are at the bottom.

#include "TestFramework.hpp"
#include "OrderBook.hpp"
#include "ArrayBitMapLocator.hpp"
#include "MatchingCore.hpp"
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>

using namespace engine;
using namespace engine::book;
using namespace engine::core;
using namespace engine::types;

static PriceRange REL_RANGE{100000, 200000, 5};
static constexpr InstrumentId REL_ID = 1;

static FastBook makeBook(std::vector<TradeEvent> &events,
                         PriceRange range = REL_RANGE)
{
    return FastBook(
        ArrayBitMapLocator(range),
        REL_ID,
        [&events](const TradeEvent &ev)
        { events.push_back(ev); });
}

static Command addLimit(OrderId oid, Verb verb, Price price, Quantity qty,
                        TimeInForce tif = TimeInForce::GTC)
{
    Command cmd{};
    cmd.type = CommandType::AddOrder;
    cmd.orderId = oid;
    cmd.instrumentId = REL_ID;
    cmd.clientId = 42;
    cmd.verb = verb;
    cmd.orderType = OrderType::Limit;
    cmd.tif = tif;
    cmd.limitPrice = price;
    cmd.qty = qty;
    return cmd;
}

static Command addMarket(OrderId oid, Verb verb, Quantity qty)
{
    Command cmd{};
    cmd.type = CommandType::AddOrder;
    cmd.orderId = oid;
    cmd.instrumentId = REL_ID;
    cmd.clientId = 42;
    cmd.verb = verb;
    cmd.orderType = OrderType::Market;
    cmd.tif = TimeInForce::IOC;
    cmd.limitPrice = NO_PRICE;
    cmd.qty = qty;
    return cmd;
}

static Command cancel(OrderId oid)
{
    Command cmd{};
    cmd.type = CommandType::CancelOrder;
    cmd.orderId = oid;
    cmd.instrumentId = REL_ID;
    cmd.clientId = 42;
    return cmd;
}

static Command modify(OrderId oid, Price newPrice, Quantity newQty, Verb verb)
{
    Command cmd{};
    cmd.type = CommandType::ModifyOrder;
    cmd.orderId = oid;
    cmd.instrumentId = REL_ID;
    cmd.clientId = 42;
    cmd.verb = verb;
    cmd.orderType = OrderType::Limit;
    cmd.tif = TimeInForce::GTC;
    cmd.limitPrice = newPrice;
    cmd.qty = newQty;
    return cmd;
}

// ═════════════════════════════════════════════════════════════════════════════
// ArrayBitMapLocator unit tests
// ═════════════════════════════════════════════════════════════════════════════

TEST("Locator: isInRange rejects out-of-range prices")
{
    ArrayBitMapLocator loc(REL_RANGE);
    ASSERT_TRUE(loc.isInRange(10000));
    ASSERT_TRUE(loc.isInRange(20000));
    ASSERT_TRUE(loc.isInRange(15000));
    ASSERT_FALSE(loc.isInRange(9999));
    ASSERT_FALSE(loc.isInRange(20001));
}

TEST("Locator: isAligned rejects off-tick prices")
{
    PriceRange range{10000, 20000, 5}; // $0.05 tick
    ArrayBitMapLocator loc(range);
    ASSERT_TRUE(loc.isAligned(10000));
    ASSERT_TRUE(loc.isAligned(10005));
    ASSERT_FALSE(loc.isAligned(10001));
    ASSERT_FALSE(loc.isAligned(10003));
}

TEST("Locator: bestBid returns NO_PRICE on empty book")
{
    ArrayBitMapLocator loc(REL_RANGE);
    ASSERT_EQ(loc.bestBid(), NO_PRICE);
}

TEST("Locator: bestAsk returns NO_PRICE on empty book")
{
    ArrayBitMapLocator loc(REL_RANGE);
    ASSERT_EQ(loc.bestAsk(), NO_PRICE);
}

TEST("Locator: markNonEmpty then bestBid returns correct price")
{
    ArrayBitMapLocator loc(REL_RANGE);
    loc.markNonEmpty(Verb::Buy, 10050);
    loc.markNonEmpty(Verb::Buy, 10030);
    ASSERT_EQ(loc.bestBid(), 10050u); // highest bid
}

TEST("Locator: markNonEmpty then bestAsk returns correct price")
{
    ArrayBitMapLocator loc(REL_RANGE);
    loc.markNonEmpty(Verb::Sell, 10060);
    loc.markNonEmpty(Verb::Sell, 10080);
    ASSERT_EQ(loc.bestAsk(), 10060u); // lowest ask
}

TEST("Locator: markEmpty clears level from bitmap")
{
    ArrayBitMapLocator loc(REL_RANGE);
    loc.markNonEmpty(Verb::Buy, 10050);
    loc.markEmpty(Verb::Buy, 10050);
    ASSERT_EQ(loc.bestBid(), NO_PRICE);
}

TEST("Locator: bitmap handles prices spanning multiple 64-bit words")
{
    // numLevels = (20000-10000)/1 + 1 = 10001, needing 157 words.
    // Test a price in word 2 (levels 128-191, prices 10128-10191).
    ArrayBitMapLocator loc(REL_RANGE);
    loc.markNonEmpty(Verb::Buy, 10150);
    loc.markNonEmpty(Verb::Buy, 10050);
    ASSERT_EQ(loc.bestBid(), 10150u); // higher price wins
    loc.markEmpty(Verb::Buy, 10150);
    ASSERT_EQ(loc.bestBid(), 10050u); // falls back to lower word
}

// ═════════════════════════════════════════════════════════════════════════════
// OrderBook: basic state tests
// ═════════════════════════════════════════════════════════════════════════════

TEST("Book: empty book has no best bid or ask")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);
    ASSERT_EQ(book.bestBid(), NO_PRICE);
    ASSERT_EQ(book.bestAsk(), NO_PRICE);
}

TEST("Book: add single resting buy — rests in book")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);
    book.addOrder(addLimit(1, Verb::Buy, 10050, 100));

    ASSERT_EQ(book.bestBid(), 10050u);
    ASSERT_EQ(book.bestAsk(), NO_PRICE);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(static_cast<int>(events[0].type),
              static_cast<int>(TradeEvent::Type::OrderAccepted));
}

TEST("Book: add single resting sell — rests in book")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);
    book.addOrder(addLimit(1, Verb::Sell, 10060, 50));

    ASSERT_EQ(book.bestAsk(), 10060u);
    ASSERT_EQ(book.bestBid(), NO_PRICE);
}

TEST("Book: best bid tracks multiple resting buys correctly")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);
    book.addOrder(addLimit(1, Verb::Buy, 10050, 100));
    book.addOrder(addLimit(2, Verb::Buy, 10070, 200));
    book.addOrder(addLimit(3, Verb::Buy, 10040, 150));

    ASSERT_EQ(book.bestBid(), 10070u); // highest
}

TEST("Book: best ask tracks multiple resting sells correctly")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);
    book.addOrder(addLimit(1, Verb::Sell, 10080, 100));
    book.addOrder(addLimit(2, Verb::Sell, 10060, 200));
    book.addOrder(addLimit(3, Verb::Sell, 10090, 150));

    ASSERT_EQ(book.bestAsk(), 10060u); // lowest
}

// ═════════════════════════════════════════════════════════════════════════════
// OrderBook: matching tests
// ═════════════════════════════════════════════════════════════════════════════

TEST("Book: crossing limit sell → full fill")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 100));
    events.clear();
    book.addOrder(addLimit(2, Verb::Sell, 10040, 100)); // crosses at 10050

    // Both orders should be fully filled.
    ASSERT_EQ(book.bestBid(), NO_PRICE);
    ASSERT_EQ(book.bestAsk(), NO_PRICE);

    // Events: one Fill event.
    bool hasFill = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::Fill)
        {
            hasFill = true;
            break;
        }
    ASSERT_TRUE(hasFill);
}

TEST("Book: fill executes at passive order's price (price-time priority)")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 100));
    events.clear();
    book.addOrder(addLimit(2, Verb::Sell, 10040, 100));

    bool correctPrice = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::Fill && ev.fillPrice == 10050u)
            correctPrice = true;
    ASSERT_TRUE(correctPrice); // fills at the resting bid price, not the ask limit
}

TEST("Book: partial fill leaves remainder in book")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 200)); // rest 200 @ 10050
    book.addOrder(addLimit(2, Verb::Sell, 10040, 80)); // sell 80 → partial fill

    ASSERT_EQ(book.bestBid(), 10050u); // still best bid
    ASSERT_EQ(book.bestAsk(), NO_PRICE);

    bool hasPartial = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::PartialFill)
        {
            hasPartial = true;
            ASSERT_EQ(ev.fillQty, 80u);
            ASSERT_EQ(ev.passiveRemaining, 120u);
        }
    ASSERT_TRUE(hasPartial);
}

TEST("Book: FIFO priority — first resting order fills first")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 100)); // first at 10050
    book.addOrder(addLimit(2, Verb::Buy, 10050, 100)); // second at 10050
    events.clear();
    book.addOrder(addLimit(3, Verb::Sell, 10040, 100)); // fills 100

    // Only order 1 should have been filled (it arrived first).
    bool order1Filled = false;
    for (auto &ev : events)
        if ((ev.type == TradeEvent::Type::Fill ||
             ev.type == TradeEvent::Type::PartialFill) &&
            ev.passiveOrderId == 1)
            order1Filled = true;
    ASSERT_TRUE(order1Filled);
}

TEST("Book: multiple price levels — best price matches first")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 100));
    book.addOrder(addLimit(2, Verb::Buy, 10060, 100)); // better bid
    events.clear();
    book.addOrder(addLimit(3, Verb::Sell, 10040, 100));

    // Should match against order 2 (higher bid = 10060).
    bool filledAt10060 = false;
    for (auto &ev : events)
        if (ev.fillPrice == 10060u)
            filledAt10060 = true;
    ASSERT_TRUE(filledAt10060);
}

TEST("Book: aggressor walks multiple price levels to fill")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Sell, 10060, 50));
    book.addOrder(addLimit(2, Verb::Sell, 10070, 50));
    events.clear();
    book.addOrder(addLimit(3, Verb::Buy, 10080, 100)); // should fill both levels

    int fills = 0;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::Fill ||
            ev.type == TradeEvent::Type::PartialFill)
            ++fills;
    ASSERT_EQ(fills, 2); // two passive orders filled
}

// ═════════════════════════════════════════════════════════════════════════════
// OrderBook: market orders
// ═════════════════════════════════════════════════════════════════════════════

TEST("Book: market order fills completely against available liquidity")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Sell, 10050, 200));
    events.clear();
    book.addOrder(addMarket(2, Verb::Buy, 200));

    bool hasFill = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::Fill)
            hasFill = true;
    ASSERT_TRUE(hasFill);
}

TEST("Book: market order against empty book → rejected")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);
    book.addOrder(addMarket(1, Verb::Buy, 100));

    bool rejected = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::OrderRejected)
            rejected = true;
    ASSERT_TRUE(rejected);
}

TEST("Book: market order partially fills then rejects remainder")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Sell, 10050, 50));
    events.clear();
    book.addOrder(addMarket(2, Verb::Buy, 200)); // only 50 available

    bool hasPartial = false, hasReject = false;
    for (auto &ev : events)
    {
        if (ev.type == TradeEvent::Type::PartialFill ||
            ev.type == TradeEvent::Type::Fill)
            hasPartial = true;
        if (ev.type == TradeEvent::Type::OrderRejected)
            hasReject = true;
    }
    ASSERT_TRUE(hasPartial);
    ASSERT_TRUE(hasReject);
}

// ═════════════════════════════════════════════════════════════════════════════
// OrderBook: IOC orders
// ═════════════════════════════════════════════════════════════════════════════

TEST("Book: IOC order with full fill succeeds")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Sell, 10050, 100));
    events.clear();
    book.addOrder(addLimit(2, Verb::Buy, 10060, 100, TimeInForce::IOC));

    bool hasFill = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::Fill)
            hasFill = true;
    ASSERT_TRUE(hasFill);
}

TEST("Book: IOC order discards unfilled remainder — does not rest")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Sell, 10050, 50));
    events.clear();
    book.addOrder(addLimit(2, Verb::Buy, 10060, 200, TimeInForce::IOC)); // 150 unfilled

    bool accepted = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::OrderAccepted)
            accepted = true;
    ASSERT_FALSE(accepted); // IOC remainder must NOT be accepted into book
}

TEST("Book: IOC with no match → nothing rests, no fill")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Sell, 10060, 100, TimeInForce::IOC));

    bool hasFill = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::Fill ||
            ev.type == TradeEvent::Type::PartialFill)
            hasFill = true;
    ASSERT_FALSE(hasFill);
}

// ═════════════════════════════════════════════════════════════════════════════
// OrderBook: cancel
// ═════════════════════════════════════════════════════════════════════════════

TEST("Book: cancel existing order removes it from book")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 100));
    events.clear();
    book.cancelOrder(cancel(1));

    ASSERT_EQ(book.bestBid(), NO_PRICE);

    bool wasCancelled = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::OrderCancelled)
            wasCancelled = true;
    ASSERT_TRUE(wasCancelled);
}

TEST("Book: cancel last order at level clears the level from bitmap")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 100));
    book.addOrder(addLimit(2, Verb::Buy, 10060, 100));
    book.cancelOrder(cancel(2));

    ASSERT_EQ(book.bestBid(), 10050u); // falls back to lower level
}

TEST("Book: cancel non-existent order → rejected, book unchanged")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 100));
    events.clear();
    book.cancelOrder(cancel(999)); // does not exist

    bool wasRejected = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::OrderRejected)
            wasRejected = true;
    ASSERT_TRUE(wasRejected);
}

TEST("Book: cancel middle order in level — list integrity preserved")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 100));
    book.addOrder(addLimit(2, Verb::Buy, 10050, 200));
    book.addOrder(addLimit(3, Verb::Buy, 10050, 300));
    book.cancelOrder(cancel(2)); // cancel the middle order

    // Verify orders 1 and 3 still fill correctly (FIFO: 1 before 3).
    events.clear();
    book.addOrder(addLimit(4, Verb::Sell, 10040, 100));
    bool order1Filled = false;
    for (auto &ev : events)
        if (ev.passiveOrderId == 1)
            order1Filled = true;
    ASSERT_TRUE(order1Filled);
}

// ═════════════════════════════════════════════════════════════════════════════
// OrderBook: modify
// ═════════════════════════════════════════════════════════════════════════════

TEST("Book: modify quantity down — preserves time priority")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 200));
    book.addOrder(addLimit(2, Verb::Buy, 10050, 100)); // arrives second

    // Reduce order 1's qty — it should still match before order 2.
    book.modifyOrder(modify(1, 10050, 80, Verb::Buy));

    events.clear();
    book.addOrder(addLimit(3, Verb::Sell, 10040, 80));

    bool order1Filled = false;
    for (auto &ev : events)
        if (ev.passiveOrderId == 1)
            order1Filled = true;
    ASSERT_TRUE(order1Filled);
}

TEST("Book: modify price — loses time priority")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 100));
    book.addOrder(addLimit(2, Verb::Buy, 10050, 100));

    // Move order 1 to same price → it goes to the tail (loses priority).
    book.modifyOrder(modify(1, 10050, 100, Verb::Buy));

    events.clear();
    book.addOrder(addLimit(3, Verb::Sell, 10040, 100));

    bool order2Filled = false;
    for (auto &ev : events)
        if (ev.passiveOrderId == 2)
            order2Filled = true;
    ASSERT_TRUE(order2Filled); // order 2 now has priority
}

TEST("Book: modify quantity up — loses time priority")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 10050, 100));
    book.addOrder(addLimit(2, Verb::Buy, 10050, 100));

    // Increase order 1's qty — re-inserted at tail.
    book.modifyOrder(modify(1, 10050, 300, Verb::Buy));

    events.clear();
    book.addOrder(addLimit(3, Verb::Sell, 10040, 100));

    bool order2Filled = false;
    for (auto &ev : events)
        if (ev.passiveOrderId == 2)
            order2Filled = true;
    ASSERT_TRUE(order2Filled);
}

TEST("Book: modify non-existent order → rejected")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.modifyOrder(modify(999, 10050, 100, Verb::Buy));

    bool wasRejected = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::OrderRejected)
            wasRejected = true;
    ASSERT_TRUE(wasRejected);
}

// ═════════════════════════════════════════════════════════════════════════════
// OrderBook: price validation
// ═════════════════════════════════════════════════════════════════════════════

TEST("Book: order with out-of-range price → rejected")
{
    std::vector<TradeEvent> events;
    auto book = makeBook(events);

    book.addOrder(addLimit(1, Verb::Buy, 25000, 100)); // $250 > max $200

    bool wasRejected = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::OrderRejected)
            wasRejected = true;
    ASSERT_TRUE(wasRejected);
}

TEST("Book: order with off-tick price → rejected")
{
    std::vector<TradeEvent> events;
    // Book with $0.05 tick
    auto book = makeBook(events, PriceRange{10000, 20000, 5});
    book.addOrder(addLimit(1, Verb::Buy, 10003, 100)); // not a multiple of 5

    bool wasRejected = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::OrderRejected)
            wasRejected = true;
    ASSERT_TRUE(wasRejected);
}

// ═════════════════════════════════════════════════════════════════════════════
// OrderBook: pool capacity
// ═════════════════════════════════════════════════════════════════════════════

TEST("Book: pool exhaustion → order rejected")
{
    // Use a tiny pool for this test.
    std::vector<TradeEvent> events;
    OrderBook<ArrayBitMapLocator, 2> tinyBook(
        ArrayBitMapLocator(REL_RANGE), REL_ID,
        [&events](const TradeEvent &ev)
        { events.push_back(ev); });

    tinyBook.addOrder(addLimit(1, Verb::Buy, 10050, 100));
    tinyBook.addOrder(addLimit(2, Verb::Buy, 10060, 100));
    events.clear();
    tinyBook.addOrder(addLimit(3, Verb::Buy, 10070, 100)); // pool full

    bool wasRejected = false;
    for (auto &ev : events)
        if (ev.type == TradeEvent::Type::OrderRejected)
            wasRejected = true;
    ASSERT_TRUE(wasRejected);
}

// ═════════════════════════════════════════════════════════════════════════════
// Integration: MatchingCore + coroutines (multi-threaded)
// ═════════════════════════════════════════════════════════════════════════════

TEST("Integration: single instrument, resting + crossing order")
{
    MatchingCore::Config cfg;
    cfg.numWorkers = 1;
    cfg.firstWorkerCore = 2;

    MatchingCore core(cfg);
    InstrumentConfig insCfg = {
        1,
        BookType::FastBook,
        REL_RANGE};

    core.addInstrument(insCfg);

    std::atomic<int> fills{0};
    core.setTradeCallback([&](const TradeEvent &ev)
                          {
        if (ev.type == TradeEvent::Type::Fill ||
            ev.type == TradeEvent::Type::PartialFill)
            ++fills; });
    core.start();

    auto wait = []
    { std::this_thread::sleep_for(std::chrono::milliseconds(30)); };

    core.submit(addLimit(1, Verb::Buy, 10050, 100));
    wait();
    core.submit(addLimit(2, Verb::Sell, 10040, 100));
    wait();

    core.stop();
    ASSERT_EQ(fills.load(), 1);
}

TEST("Integration: multiple instruments routed independently")
{
    MatchingCore::Config cfg;
    cfg.numWorkers = 2;
    cfg.firstWorkerCore = 2;

    MatchingCore core(cfg);
    InstrumentConfig insCfg1 = {
        1,
        BookType::FastBook,
        REL_RANGE};

    InstrumentConfig insCfg2 = {
        2,
        BookType::FastBook,
        {5000, 15000, 1}};

    core.addInstrument(insCfg1);
    core.addInstrument(insCfg2);

    std::atomic<int> fills{0};
    core.setTradeCallback([&](const TradeEvent &ev)
                          {
        if (ev.type == TradeEvent::Type::Fill) ++fills; });
    core.start();

    auto wait = []
    { std::this_thread::sleep_for(std::chrono::milliseconds(30)); };

    core.submit(addLimit(1, Verb::Buy, 10050, 100)); // instrument 1
    core.submit(addLimit(2, Verb::Buy, 6000, 50));   // instrument 2
    wait();
    auto sell1 = addLimit(3, Verb::Sell, 10040, 100);
    sell1.instrumentId = 1;
    auto sell2 = addLimit(4, Verb::Sell, 5900, 50);
    sell2.instrumentId = 2;
    core.submit(sell1);
    core.submit(sell2);
    wait();

    core.stop();
    ASSERT_EQ(fills.load(), 2);
}

TEST("Integration: submit to unknown instrument returns false")
{
    MatchingCore::Config cfg;
    cfg.numWorkers = 1;
    cfg.firstWorkerCore = 2;
    MatchingCore core(cfg);

    InstrumentConfig insCfg = {
        1,
        BookType::FastBook,
        REL_RANGE};

    core.addInstrument(insCfg);
    core.start();

    auto badCmd = addLimit(1, Verb::Buy, 10050, 100);
    badCmd.instrumentId = 999; // not registered
    ASSERT_FALSE(core.submit(badCmd));

    core.stop();
}

TEST("Integration: stress — 10K orders, all match")
{
    MatchingCore::Config cfg;
    cfg.numWorkers = 2;
    cfg.firstWorkerCore = 2;

    MatchingCore core(cfg);

    InstrumentConfig insCfg = {
        1,
        BookType::FastBook,
        REL_RANGE};
    core.addInstrument(insCfg);

    std::atomic<uint64_t> totalFillQty{0};
    core.setTradeCallback([&](const TradeEvent &ev)
                          {
        if (ev.type == TradeEvent::Type::Fill ||
            ev.type == TradeEvent::Type::PartialFill)
            totalFillQty += ev.fillQty; });
    core.start();

    static constexpr int N = 5000;
    for (int i = 0; i < N; ++i)
    {
        core.submit(addLimit(i * 2 + 1, Verb::Buy, 10050, 10));
        core.submit(addLimit(i * 2 + 2, Verb::Sell, 10040, 10));
    }

    // Wait for processing.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    core.stop();

    ASSERT_EQ(totalFillQty.load(), static_cast<uint64_t>(N * 10));
}

int main()
{
    return test::TestRegistry::run();
}