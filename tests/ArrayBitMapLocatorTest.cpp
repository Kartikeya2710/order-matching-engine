#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ArrayBitMapLocator.hpp"
#include "OrderPool.hpp"

using namespace engine;
using namespace engine::book;

namespace
{
    // The locator finds a price's level with a multiply by a stored reciprocal of the tick size
    // instead of a divide. These tests check it against plain division.

    // 1: the stored reciprocal is UINT64_MAX, the largest value it can hold.
    // 49 and 103: a double reciprocal with truncation puts some of their prices one level low.
    constexpr std::array<types::Price, 9> TICKS = {1, 2, 3, 5, 7, 49, 64, 103, 65537};

    // Ranges whose offsets from minPrice come close to the largest Price, where the multiply's
    // error is largest. maxPrice stays below NO_PRICE, as the instrument config requires.
    constexpr std::array<PriceRange, 4> TOP_RANGES = {{
        {.minPrice = 0, .maxPrice = 4294967294, .tickSize = 2147483647}, // 3 levels
        {.minPrice = 0, .maxPrice = 4294967294, .tickSize = 4294967294}, // 2 levels, largest tick
        {.minPrice = 0, .maxPrice = 4273492563, .tickSize = 21474837},   // 200 levels
        {.minPrice = 4294967095, .maxPrice = 4294967294, .tickSize = 1}, // 200 levels at the top
    }};

    // 200 levels span four bitmap words. minPrice is at least one tick, so the price one tick
    // below it exists.
    PriceRange rangeWithTick(types::Price tick)
    {
        return {.minPrice = 1'000'000, .maxPrice = 1'000'000 + 199 * tick, .tickSize = tick};
    }

    std::string describe(const PriceRange& range)
    {
        return "range " + std::to_string(range.minPrice) + ".." + std::to_string(range.maxPrice) +
               " tick " + std::to_string(range.tickSize);
    }

    // isValidPrice() as it was written with % before the multiply replaced the divide.
    bool validByModulo(const PriceRange& range, types::Price price)
    {
        return price >= range.minPrice && price <= range.maxPrice &&
               (price - range.minPrice) % range.tickSize == 0;
    }

    // Prices from `first` to `last`, both included. uint64_t so that a loop ending at the largest
    // Price stops.
    struct PriceInterval
    {
        uint64_t first;
        uint64_t last;
    };

    // Expects isValidPrice() to agree with validByModulo() for every price in `prices`.
    // Reports only the first price that disagrees, so a broken build prints one line per call.
    void expectValidityMatchesModulo(
        const ArrayBitMapLocator& locator,
        const PriceRange& range,
        PriceInterval prices
    )
    {
        for (uint64_t p = prices.first; p <= prices.last; ++p)
        {
            const auto price = static_cast<types::Price>(p);
            if (locator.isValidPrice(price) != validByModulo(range, price))
            {
                ADD_FAILURE() << "isValidPrice(" << price << ") disagrees with % for "
                              << describe(range);
                return;
            }
        }
    }

    // Rests one order at every price of `range` on `side`, then checks that:
    // - each price reaches the level holding its own order, so no two prices share a level;
    // - walking from the best price visits every price once, best first;
    // - removing the best order each time moves the best price one level on.
    // remove() also asserts in debug builds that the order's price maps to the level it rests in.
    void expectEveryPriceHasItsOwnLevel(const PriceRange& range, types::Verb side)
    {
        SCOPED_TRACE(describe(range) + (side == types::Verb::Buy ? " buy" : " sell"));
        const bool isBuy = (side == types::Verb::Buy);
        ArrayBitMapLocator locator(range);
        OrderPool<256> pool;

        // Best price first. uint64_t so that stepping past the largest Price ends the loop.
        std::vector<types::Price> prices;
        for (uint64_t p = range.minPrice; p <= range.maxPrice; p += range.tickSize)
        {
            prices.push_back(static_cast<types::Price>(p));
        }
        if (isBuy)
        {
            std::ranges::reverse(prices);
        }
        ASSERT_LE(prices.size(), 256u);

        std::vector<uint32_t> slotIdxs;
        for (const types::Price price : prices)
        {
            slotIdxs.push_back(pool.acquire(
                {.orderId = static_cast<types::OrderId>(slotIdxs.size() + 1),
                 .verb = side,
                 .clientId = 1,
                 .price = price,
                 .qty = 10}
            ));
            locator.append(slotIdxs.back(), pool.slots());
        }

        for (size_t i = 0; i < prices.size(); ++i)
        {
            const PriceLevel& level = locator.level(side, prices[i]);
            EXPECT_EQ(level.head, slotIdxs[i]) << "price " << prices[i];
            EXPECT_EQ(level.orderCount, 1u) << "price " << prices[i];
        }

        // The size check stops a walk that never ends.
        std::vector<types::Price> walked;
        for (types::Price p = isBuy ? locator.bestBid() : locator.bestAsk();
             p != NO_PRICE && walked.size() <= prices.size();
             p = isBuy ? locator.nextBid(p) : locator.nextAsk(p))
        {
            walked.push_back(p);
        }
        EXPECT_EQ(walked, prices);

        for (size_t i = 0; i < prices.size(); ++i)
        {
            locator.remove(slotIdxs[i], pool.slots());
            const types::Price nextBest = (i + 1 < prices.size()) ? prices[i + 1] : NO_PRICE;
            EXPECT_EQ(isBuy ? locator.bestBid() : locator.bestAsk(), nextBest);
        }
    }

    TEST(ArrayBitMapLocatorTest, IsValidPriceMatchesModuloAtEveryPrice)
    {
        for (const types::Price tick : TICKS)
        {
            const PriceRange range = rangeWithTick(tick);
            const ArrayBitMapLocator locator(range);

            expectValidityMatchesModulo(
                locator,
                range,
                {.first = range.minPrice - tick, .last = uint64_t{range.maxPrice} + tick}
            );
        }
    }

    TEST(ArrayBitMapLocatorTest, IsValidPriceMatchesModuloNearLargestPrice)
    {
        for (const PriceRange& range : TOP_RANGES)
        {
            const ArrayBitMapLocator locator(range);

            // Each level's price and the prices either side. Above maxPrice that reaches
            // NO_PRICE, which must be invalid.
            for (uint64_t p = range.minPrice; p <= range.maxPrice; p += range.tickSize)
            {
                expectValidityMatchesModulo(
                    locator,
                    range,
                    {.first = (p == 0) ? 0 : p - 1, .last = p + 1}
                );
            }
        }
    }

    TEST(ArrayBitMapLocatorTest, EveryPriceHasItsOwnLevel)
    {
        for (const types::Price tick : TICKS)
        {
            expectEveryPriceHasItsOwnLevel(rangeWithTick(tick), types::Verb::Buy);
            expectEveryPriceHasItsOwnLevel(rangeWithTick(tick), types::Verb::Sell);
        }
    }

    TEST(ArrayBitMapLocatorTest, EveryPriceHasItsOwnLevelNearLargestPrice)
    {
        for (const PriceRange& range : TOP_RANGES)
        {
            expectEveryPriceHasItsOwnLevel(range, types::Verb::Buy);
            expectEveryPriceHasItsOwnLevel(range, types::Verb::Sell);
        }
    }
} // namespace
