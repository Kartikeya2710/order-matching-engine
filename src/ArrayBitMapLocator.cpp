#include "ArrayBitMapLocator.hpp"

namespace engine::book
{
    uint32_t ArrayBitMapLocator::priceToIndex(types::Price price) const noexcept
    {
        return static_cast<uint32_t>((price - range_.minPrice) / range_.tickSize);
    }

    types::Price ArrayBitMapLocator::indexToPrice(uint32_t idx) const noexcept
    {
        return range_.minPrice + idx * range_.tickSize;
    }

    std::vector<PriceLevel> &ArrayBitMapLocator::priceLevels(types::Verb verb) noexcept
    {
        return (verb == types::Verb::Buy) ? bids_ : asks_;
    }

    std::vector<uint64_t> &ArrayBitMapLocator::bitMapFor(types::Verb verb) noexcept
    {
        return (verb == types::Verb::Buy) ? bidBitMap_ : askBitMap_;
    }

    const std::vector<uint64_t> &ArrayBitMapLocator::bitMapFor(types::Verb verb) const noexcept
    {
        return (verb == types::Verb::Buy) ? bidBitMap_ : askBitMap_;
    }

    void ArrayBitMapLocator::setBit(std::vector<uint64_t> &bm, uint32_t idx) noexcept
    {
        bm[idx / BITMAP_WORD_SIZE] |= (uint64_t{1} << (idx % BITMAP_WORD_SIZE));
    }

    void ArrayBitMapLocator::clearBit(std::vector<uint64_t> &bm, uint32_t idx) noexcept
    {
        bm[idx / BITMAP_WORD_SIZE] &= ~(uint64_t{1} << (idx % BITMAP_WORD_SIZE));
    }

    ArrayBitMapLocator::ArrayBitMapLocator(PriceRange range)
        : range_(range), numLevels_(((range.maxPrice - range.minPrice) / range.tickSize) + 1), bids_(numLevels_), asks_(numLevels_), bidBitMap_((numLevels_ + BITMAP_WORD_SIZE - 1) / BITMAP_WORD_SIZE, uint64_t{0}), askBitMap_((numLevels_ + BITMAP_WORD_SIZE - 1) / BITMAP_WORD_SIZE, uint64_t{0})
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
        for (int w = static_cast<int>(bidBitMap_.size()) - 1; w >= 0; --w)
        {
            if (bidBitMap_[w] == 0)
                continue;
            std::uint32_t bit = 63u - static_cast<std::uint32_t>(__builtin_clzll(bidBitMap_[w]));
            return indexToPrice(static_cast<std::uint32_t>(w) * BITMAP_WORD_SIZE + bit);
        }
        return NO_PRICE;
    }

    types::Price ArrayBitMapLocator::bestAsk() const noexcept
    {
        for (size_t w = 0; w < askBitMap_.size(); ++w)
        {
            if (askBitMap_[w] == 0)
                continue;
            std::uint32_t bit = static_cast<std::uint32_t>(__builtin_ctzll(askBitMap_[w]));
            return indexToPrice(static_cast<std::uint32_t>(w) * BITMAP_WORD_SIZE + bit);
        }
        return NO_PRICE;
    }

    types::Price ArrayBitMapLocator::nextBid(types::Price price) const noexcept
    {
        if (price <= range_.minPrice)
            return NO_PRICE;

        uint32_t startIdx = priceToIndex(price) - 1;
        int startWord = static_cast<int>(startIdx / BITMAP_WORD_SIZE);
        uint32_t startBit = startIdx % BITMAP_WORD_SIZE;
        uint64_t mask = (startBit == 63u) ? ~uint64_t{0} : (uint64_t{1} << (startBit + 1)) - 1;

        for (int w = startWord; w >= 0; --w, mask = ~uint64_t{0})
        {
            uint64_t word = bidBitMap_[w] & mask;
            if (word == 0)
                continue;
            uint32_t bit = 63u - static_cast<uint32_t>(__builtin_clzll(word));
            return indexToPrice(static_cast<uint32_t>(w) * BITMAP_WORD_SIZE + bit);
        }

        return NO_PRICE;
    }

    types::Price ArrayBitMapLocator::nextAsk(types::Price price) const noexcept
    {
        uint32_t startIdx = priceToIndex(price) + 1;
        if (startIdx >= static_cast<uint32_t>(numLevels_))
            return NO_PRICE;

        size_t startWord = startIdx / BITMAP_WORD_SIZE;
        uint32_t startBit = startIdx % BITMAP_WORD_SIZE;
        uint64_t mask = ~((uint64_t{1} << startBit) - 1);

        for (size_t w = startWord; w < askBitMap_.size(); ++w, mask = ~uint64_t{0})
        {
            uint64_t word = askBitMap_[w] & mask;
            if (word == 0)
                continue;
            uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(word));
            return indexToPrice(static_cast<uint32_t>(w) * BITMAP_WORD_SIZE + bit);
        }

        return NO_PRICE;
    }
}
