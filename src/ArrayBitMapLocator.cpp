#include "ArrayBitMapLocator.hpp"

namespace engine::book
{
    std::uint32_t ArrayBitMapLocator::priceToIndex(types::Price price) const noexcept
    {
        return static_cast<std::uint32_t>((price - range_.minPrice) / range_.tickSize);
    }

    types::Price ArrayBitMapLocator::indexToPrice(std::uint32_t idx) const noexcept
    {
        return range_.minPrice + idx * range_.tickSize;
    }

    std::vector<PriceLevel> &ArrayBitMapLocator::priceLevels(types::Verb verb) noexcept
    {
        return (verb == types::Verb::Buy) ? bids_ : asks_;
    }

    std::vector<std::uint32_t> &ArrayBitMapLocator::bitMapFor(types::Verb verb) noexcept
    {
        return (verb == types::Verb::Buy) ? bidBitMap_ : askBitMap_;
    }

    const std::vector<std::uint32_t> &ArrayBitMapLocator::bitMapFor(types::Verb verb) const noexcept
    {
        return (verb == types::Verb::Buy) ? bidBitMap_ : askBitMap_;
    }

    void ArrayBitMapLocator::setBit(std::vector<std::uint32_t> &bm, std::uint32_t idx) noexcept
    {
        bm[idx / BITMAP_WORD_SIZE] |= (1ULL << (idx % BITMAP_WORD_SIZE));
    }

    void ArrayBitMapLocator::clearBit(std::vector<std::uint32_t> &bm, std::uint32_t idx) noexcept
    {
        bm[idx / BITMAP_WORD_SIZE] &= ~(1ULL << (idx % BITMAP_WORD_SIZE));
    }

    ArrayBitMapLocator::ArrayBitMapLocator(PriceRange range)
        : range_(range), numLevels_(((range.maxPrice - range.minPrice) / range.tickSize) + 1), bids_(numLevels_), asks_(numLevels_), bidBitMap_((numLevels_ + BITMAP_WORD_SIZE - 1) / BITMAP_WORD_SIZE, std::uint32_t{0}), askBitMap_((numLevels_ + BITMAP_WORD_SIZE - 1) / BITMAP_WORD_SIZE, std::uint32_t{0})
    {
    }

    PriceLevel &ArrayBitMapLocator::getPriceLevel(types::Verb verb, types::Price price) noexcept
    {
        return priceLevels(verb)[priceToIndex(price)];
    }

    void ArrayBitMapLocator::markEmpty(types::Verb side, types::Price price) noexcept
    {
        clearBit(bitMapFor(side), priceToIndex(price));
    }

    void ArrayBitMapLocator::markNonEmpty(types::Verb side, types::Price price) noexcept
    {
        setBit(bitMapFor(side), priceToIndex(price));
    }

    bool ArrayBitMapLocator::isInRange(types::Price price) const noexcept
    {
        return price >= range_.minPrice && price <= range_.maxPrice;
    }

    bool ArrayBitMapLocator::isAligned(types::Price price) const noexcept
    {
        return (price - range_.minPrice) % range_.tickSize == 0;
    }

    types::Price ArrayBitMapLocator::bestBid() const noexcept
    {
        const auto &bm = bitMapFor(types::Verb::Buy);
        for (int w = static_cast<int>(bm.size()) - 1; w >= 0; --w)
        {
            if (bm[static_cast<size_t>(w)] == 0)
                continue;
            std::uint32_t bit = 63u - static_cast<std::uint32_t>(__builtin_clzll(bm[static_cast<size_t>(w)]));
            return indexToPrice(static_cast<std::uint32_t>(w) * BITMAP_WORD_SIZE + bit);
        }
        return NO_PRICE;
    }

    types::Price ArrayBitMapLocator::bestAsk() const noexcept
    {
        const auto &bm = bitMapFor(types::Verb::Sell);
        for (size_t w = 0; w < bm.size(); ++w)
        {
            if (bm[w] == 0)
                continue;
            std::uint32_t bit = static_cast<std::uint32_t>(__builtin_ctzll(bm[w]));
            return indexToPrice(static_cast<std::uint32_t>(w) * BITMAP_WORD_SIZE + bit);
        }
        return NO_PRICE;
    }
}
