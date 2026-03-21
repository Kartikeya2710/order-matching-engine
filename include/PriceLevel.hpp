#pragma once
#include <cstdint>
#include "Types.hpp"
#include "OrderPool.hpp"

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

    // Free functions — take pool by reference so PriceLevel stays a plain struct.
    template <size_t PoolCap>
    void appendOrderInLevel(PriceLevel &level, std::uint32_t poolIdx, OrderPool<PoolCap> &pool) noexcept
    {
        pool[poolIdx].next = NULL_IDX;
        pool[poolIdx].prev = level.tail;

        if (level.tail != NULL_IDX)
            pool[level.tail].next = poolIdx;
        else
            level.head = poolIdx;

        level.tail = poolIdx;
        level.totalQty += pool[poolIdx].quantity;
        ++level.orderCount;
    }

    template <size_t PoolCap>
    void removeOrderFromLevel(PriceLevel &level, std::uint32_t poolIdx, OrderPool<PoolCap> &pool) noexcept
    {
        auto &order = pool[poolIdx];

        if (order.prev != NULL_IDX)
            pool[order.prev].next = order.next;
        else
            level.head = order.next;

        if (order.next != NULL_IDX)
            pool[order.next].prev = order.prev;
        else
            level.tail = order.prev;

        level.totalQty -= order.quantity;
        --level.orderCount;
    }
}