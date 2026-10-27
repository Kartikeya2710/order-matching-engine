#include "ArrayBitMapLocator.hpp"
#include <bit>
#include <cassert>

namespace engine::book
{
    uint32_t ArrayBitMapLocator::priceToIndex(types::Price price) const noexcept
    {
        assert(isValidPrice(price));
        return static_cast<uint32_t>((price - range_.minPrice) / range_.tickSize);
    }

    types::Price ArrayBitMapLocator::indexToPrice(uint32_t idx) const noexcept
    {
        assert(idx < numLevels_);
        return range_.minPrice + idx * range_.tickSize;
    }

    std::vector<PriceLevel>& ArrayBitMapLocator::priceLevels(types::Verb verb) noexcept
    {
        return (verb == types::Verb::Buy) ? bids_ : asks_;
    }

    const std::vector<PriceLevel>& ArrayBitMapLocator::priceLevels(types::Verb verb) const noexcept
    {
        return (verb == types::Verb::Buy) ? bids_ : asks_;
    }

    std::vector<uint64_t>& ArrayBitMapLocator::bitMapFor(types::Verb verb) noexcept
    {
        return (verb == types::Verb::Buy) ? bidBitMap_ : askBitMap_;
    }

    void ArrayBitMapLocator::setBit(std::vector<uint64_t>& bm, uint32_t idx) noexcept
    {
        assert(idx < numLevels_);
        bm[idx / BITMAP_WORD_SIZE] |= (uint64_t{1} << (idx % BITMAP_WORD_SIZE));
    }

    void ArrayBitMapLocator::clearBit(std::vector<uint64_t>& bm, uint32_t idx) noexcept
    {
        assert(idx < numLevels_);
        bm[idx / BITMAP_WORD_SIZE] &= ~(uint64_t{1} << (idx % BITMAP_WORD_SIZE));
    }

    ArrayBitMapLocator::ArrayBitMapLocator(PriceRange range)
        : range_(range),
          numLevels_(((range.maxPrice - range.minPrice) / range.tickSize) + 1),
          bids_(numLevels_),
          asks_(numLevels_),
          bidBitMap_((numLevels_ + BITMAP_WORD_SIZE - 1) / BITMAP_WORD_SIZE, uint64_t{0}),
          askBitMap_((numLevels_ + BITMAP_WORD_SIZE - 1) / BITMAP_WORD_SIZE, uint64_t{0}),
          bestBid_(NO_PRICE),
          bestAsk_(NO_PRICE)
    {
    }

    const PriceLevel& ArrayBitMapLocator::level(types::Verb side, types::Price price) const noexcept
    {
        return priceLevels(side)[priceToIndex(price)];
    }

    void ArrayBitMapLocator::append(uint32_t slotIdx, std::span<PoolSlot> slots) noexcept
    {
        const Order& order = slots[slotIdx].order();
        const uint32_t idx = priceToIndex(order.price);
        priceLevels(order.verb)[idx].append(slotIdx, slots);
        setBit(bitMapFor(order.verb), idx);

        if (order.verb == types::Verb::Buy)
        {
            if (bestBid_ == NO_PRICE || order.price > bestBid_)
            {
                bestBid_ = order.price;
            }
        }
        else if (order.price < bestAsk_) // NO_PRICE is the largest Price, so no extra check
        {
            bestAsk_ = order.price;
        }
    }

    void ArrayBitMapLocator::remove(uint32_t slotIdx, std::span<PoolSlot> slots) noexcept
    {
        const Order& order = slots[slotIdx].order();
        const uint32_t idx = priceToIndex(order.price);
        PriceLevel& level = priceLevels(order.verb)[idx];
        level.remove(slotIdx, slots);

        if (level.empty())
        {
            clearBit(bitMapFor(order.verb), idx);

            // Only emptying the best level moves the best price.
            if (order.verb == types::Verb::Buy)
            {
                if (order.price == bestBid_)
                {
                    bestBid_ = highestBidBelow(idx);
                }
            }
            else if (order.price == bestAsk_)
            {
                bestAsk_ = lowestAskAbove(idx);
            }
        }
    }

    void ArrayBitMapLocator::reduce(
        uint32_t slotIdx,
        types::Quantity by,
        std::span<PoolSlot> slots
    ) noexcept
    {
        const Order& order = slots[slotIdx].order();
        priceLevels(order.verb)[priceToIndex(order.price)].reduce(slotIdx, by, slots);
    }

    bool ArrayBitMapLocator::isValidPrice(types::Price price) const noexcept
    {
        return (price >= range_.minPrice && price <= range_.maxPrice) &&
               (price - range_.minPrice) % range_.tickSize == 0;
    }

    types::Price ArrayBitMapLocator::bestBid() const noexcept
    {
        return bestBid_;
    }

    types::Price ArrayBitMapLocator::bestAsk() const noexcept
    {
        return bestAsk_;
    }

    // At minPrice / maxPrice the scan finds no level and returns NO_PRICE, so the edges need no
    // check of their own.
    types::Price ArrayBitMapLocator::nextBid(types::Price price) const noexcept
    {
        return highestBidBelow(priceToIndex(price));
    }

    types::Price ArrayBitMapLocator::nextAsk(types::Price price) const noexcept
    {
        return lowestAskAbove(priceToIndex(price));
    }

    types::Price ArrayBitMapLocator::highestBidBelow(uint32_t idx) const noexcept
    {
        assert(idx < numLevels_);
        uint32_t w = idx / BITMAP_WORD_SIZE;
        // Set the bits below the price level within the word.
        uint64_t word = bidBitMap_[w] & ((uint64_t{1} << (idx % BITMAP_WORD_SIZE)) - 1);
        while (word == 0)
        {
            if (w == 0)
            {
                return NO_PRICE;
            }
            word = bidBitMap_[--w];
        }
        return indexToPrice(w * BITMAP_WORD_SIZE + std::bit_width(word) - 1);
    }

    types::Price ArrayBitMapLocator::lowestAskAbove(uint32_t idx) const noexcept
    {
        assert(idx < numLevels_);
        // Set the bits above the price level within the word. It is 0 when b is 63. Higher words
        // keep every bit.
        uint64_t mask = ~uint64_t{1} << (idx % BITMAP_WORD_SIZE);

        for (uint32_t w = idx / BITMAP_WORD_SIZE; w < askBitMap_.size(); ++w, mask = ~uint64_t{0})
        {
            const uint64_t word = askBitMap_[w] & mask;
            if (word != 0)
            {
                return indexToPrice(w * BITMAP_WORD_SIZE + std::countr_zero(word));
            }
        }

        return NO_PRICE;
    }
} // namespace engine::book
