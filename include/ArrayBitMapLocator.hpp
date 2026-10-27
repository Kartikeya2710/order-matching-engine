#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "OrderPool.hpp"
#include "PriceLevel.hpp"
#include "Types.hpp"

namespace engine::book
{

    struct PriceRange
    {
        types::Price minPrice;
        types::Price maxPrice;
        types::Price tickSize;
    };

    // Each bitmap word is 64 bits — one word covers 64 consecutive price levels.
    static constexpr uint32_t BITMAP_WORD_SIZE = 64;

    // A level's bit is set exactly when the level has orders.
    // `append()`, `remove()` and `reduce()` are the only ways to change a level
    class ArrayBitMapLocator : private LevelHandleReader
    {
    private:
        PriceRange range_;
        size_t numLevels_;
        std::vector<PriceLevel> bids_;
        std::vector<PriceLevel> asks_;
        std::vector<uint64_t> bidBitMap_;
        std::vector<uint64_t> askBitMap_;
        // NO_PRICE when the side is empty
        types::Price bestBid_;
        types::Price bestAsk_;

        // `price` must pass isValidPrice(). Every `idx` taken below must be valid
        [[nodiscard]] uint32_t priceToIndex(types::Price price) const noexcept;
        [[nodiscard]] types::Price indexToPrice(uint32_t idx) const noexcept;

        // Nearest live level strictly below / above `idx`, or NO_PRICE if there is none.
        [[nodiscard]] types::Price highestBidBelow(uint32_t idx) const noexcept;
        [[nodiscard]] types::Price lowestAskAbove(uint32_t idx) const noexcept;

        std::vector<PriceLevel>& priceLevels(types::Verb verb) noexcept;
        [[nodiscard]] const std::vector<PriceLevel>& priceLevels(types::Verb verb) const noexcept;

        std::vector<uint64_t>& bitMapFor(types::Verb verb) noexcept;

        void setBit(std::vector<uint64_t>& bm, uint32_t idx) noexcept;
        void clearBit(std::vector<uint64_t>& bm, uint32_t idx) noexcept;

    public:
        explicit ArrayBitMapLocator(PriceRange range);

        // The order book calls it on every price a request brings in, before the
        // price is matched or stored, so prices received by the methods below are valid.
        [[nodiscard]] bool isValidPrice(types::Price price) const noexcept;

        // Read-only view of a level, for walking its queue.
        [[nodiscard]] const PriceLevel& level(types::Verb side, types::Price price) const noexcept;

        // The order's side and price are read from its slot, so they must not change while the
        // order rests.
        void append(uint32_t slotIdx, std::span<PoolSlot> slots) noexcept;
        void remove(uint32_t slotIdx, std::span<PoolSlot> slots) noexcept;
        void reduce(uint32_t slotIdx, types::Quantity by, std::span<PoolSlot> slots) noexcept;

        // Nearest live level strictly below / above `price`, or NO_PRICE if there is none.
        [[nodiscard]] types::Price nextBid(types::Price price) const noexcept;
        [[nodiscard]] types::Price nextAsk(types::Price price) const noexcept;

        [[nodiscard]] types::Price bestBid() const noexcept;
        [[nodiscard]] types::Price bestAsk() const noexcept;
    };

} // namespace engine::book
