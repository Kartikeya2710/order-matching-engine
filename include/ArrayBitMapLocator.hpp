#include "PriceLevel.hpp"
#include "Types.hpp"
#include <cstddef>
#include <vector>

namespace engine::book
{

    struct PriceRange
    {
        types::Price minPrice;
        types::Price maxPrice;
        types::Price tickSize;
    };

    static constexpr std::uint32_t BITMAP_WORD_SIZE = 64;

    class ArrayBitMapLocator
    {
    private:
        PriceRange range_;
        size_t numLevels_;
        std::vector<PriceLevel> bids_;
        std::vector<PriceLevel> asks_;
        std::vector<std::uint32_t> bidBitMap_;
        std::vector<std::uint32_t> askBitMap_;

        std::uint32_t priceToIndex(types::Price price) const noexcept;

        types::Price indexToPrice(std::uint32_t idx) const noexcept;

        std::vector<PriceLevel> &priceLevels(types::Verb verb) noexcept;

        std::vector<std::uint32_t> &bitMapFor(types::Verb verb) noexcept;

        const std::vector<std::uint32_t> &bitMapFor(types::Verb verb) const noexcept;

        void setBit(std::vector<std::uint32_t> &bm, std::uint32_t idx) noexcept;

        void clearBit(std::vector<std::uint32_t> &bm, std::uint32_t idx) noexcept;

    public:
        explicit ArrayBitMapLocator(PriceRange range);

        PriceLevel &getPriceLevel(types::Verb verb, types::Price price) noexcept;

        void markEmpty(types::Verb side, types::Price price) noexcept;

        void markNonEmpty(types::Verb side, types::Price price) noexcept;

        bool isInRange(types::Price price) const noexcept;

        bool isAligned(types::Price price) const noexcept;

        types::Price bestBid() const noexcept;

        types::Price bestAsk() const noexcept;
    };

}
