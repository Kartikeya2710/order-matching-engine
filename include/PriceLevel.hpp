#pragma once
#include <cstdint>
#include "Types.hpp"

namespace engine::book
{
    template <uint32_t C>
    class OrderPool;
}

namespace engine::book
{

    static constexpr uint32_t NULL_IDX = UINT32_MAX;

    // NO_PRICE is returned by bestBid/bestAsk when the side is empty.
    static constexpr types::Price NO_PRICE = types::Price(-1);

    struct PriceLevel
    {
        uint32_t head = NULL_IDX;
        uint32_t tail = NULL_IDX;
        types::Quantity totalQty = 0;
        uint32_t orderCount = 0;

        bool empty() const noexcept { return head == NULL_IDX; }
    };

    // Free functions — take pool by reference so PriceLevel stays a plain struct.
    template <uint32_t PoolCap>
    void appendOrderInLevel(PriceLevel &level, uint32_t poolIdx,
                            OrderPool<PoolCap> &pool) noexcept
    {
        pool[poolIdx].next = NULL_IDX;
        pool[poolIdx].prev = level.tail;

        if (level.tail != NULL_IDX)
            pool[level.tail].next = poolIdx;
        else
            level.head = poolIdx;

        level.tail = poolIdx;
        level.totalQty += pool[poolIdx].qty;
        ++level.orderCount;
    }

    template <uint32_t PoolCap>
    void removeOrderFromLevel(PriceLevel &level, uint32_t poolIdx,
                              OrderPool<PoolCap> &pool) noexcept
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

        level.totalQty -= order.qty;
        --level.orderCount;
    }
}