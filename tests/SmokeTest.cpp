// Proves the test pipeline is wired up: GoogleTest builds, links against the
// engine library and is discovered by CTest. Real tests replace this file.

#include <gtest/gtest.h>

#include "ArrayBitMapLocator.hpp"

using namespace engine::book;

TEST(Smoke, EmptyLocatorHasNoBestPrices)
{
    ArrayBitMapLocator locator(PriceRange{.minPrice = 10000, .maxPrice = 20000, .tickSize = 5});

    EXPECT_EQ(locator.bestBid(), NO_PRICE);
    EXPECT_EQ(locator.bestAsk(), NO_PRICE);
}
