#pragma once
#include <cstdint>
#include "Types.hpp"

namespace engine::book
{
    static constexpr std::uint32_t NULL_IDX = UINT32_MAX;
    static constexpr types::Price NO_PRICE = types::Price(-1);

    struct PriceLevel
    {
        std::uint32_t head = NULL_IDX;
        std::uint32_t tail = NULL_IDX;
        types::Quantity totalQty = 0;
        std::uint32_t orderCount = 0;

        bool empty() const noexcept { return head == NULL_IDX; }
    };
}