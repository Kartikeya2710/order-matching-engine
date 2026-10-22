#pragma once
#include <cassert>
#include <cstdint>
#include <span>

#include "OrderPool.hpp"
#include "Types.hpp"

namespace engine::book
{

    // NO_PRICE is returned by bestBid/bestAsk when the side is empty.
    static constexpr types::Price NO_PRICE = types::Price(-1);

    // A FIFO queue of resting orders at one price. The queue is a linked list threaded through
    // the pool's slots (PoolSlot's next and prev), so each operation takes the slot array.
    struct PriceLevel
    {
        uint32_t head = NULL_IDX;
        uint32_t tail = NULL_IDX;
        types::Quantity totalQty = 0;
        uint32_t orderCount = 0;

        [[nodiscard]] bool empty() const noexcept
        {
            return head == NULL_IDX;
        }

        // Adds the order in `slotIdx` to the back of the queue.
        void append(uint32_t slotIdx, std::span<PoolSlot> slots) noexcept
        {
            assert(slots[slotIdx].order_.qty > 0);

            slots[slotIdx].next_ = NULL_IDX;
            slots[slotIdx].prev_ = tail;

            if (tail != NULL_IDX)
            {
                slots[tail].next_ = slotIdx;
            }
            else
            {
                head = slotIdx;
            }

            tail = slotIdx;
            totalQty += slots[slotIdx].order_.qty;
            ++orderCount;
        }

        // Takes the order in `slotIdx` out of the queue. Does not free the slot.
        void remove(uint32_t slotIdx, std::span<PoolSlot> slots) noexcept
        {
            const PoolSlot& removed = slots[slotIdx];

            // The order must be in this queue: its neighbours, or head and tail, point back at it.
            assert(
                removed.prev_ != NULL_IDX ? slots[removed.prev_].next_ == slotIdx : head == slotIdx
            );
            assert(
                removed.next_ != NULL_IDX ? slots[removed.next_].prev_ == slotIdx : tail == slotIdx
            );

            if (removed.prev_ != NULL_IDX)
            {
                slots[removed.prev_].next_ = removed.next_;
            }
            else
            {
                head = removed.next_;
            }

            if (removed.next_ != NULL_IDX)
            {
                slots[removed.next_].prev_ = removed.prev_;
            }
            else
            {
                tail = removed.prev_;
            }

            totalQty -= slots[slotIdx].order_.qty;
            --orderCount;
        }

        // Takes `by` off the order in `slotIdx`.
        // Taking an order to zero must be a remove, not a reduce.
        void reduce(uint32_t slotIdx, types::Quantity by, std::span<PoolSlot> slots) noexcept
        {
            slots[slotIdx].order_.qty -= by;
            totalQty -= by;
        }
    };

} // namespace engine::book