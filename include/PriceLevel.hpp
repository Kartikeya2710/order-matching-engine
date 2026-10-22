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
    // the pool's slots (PoolOrder::levelLinks), so each operation takes the slot array.
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

        // Adds the order in `slot` to the back of the queue.
        void append(uint32_t slot, std::span<PoolOrder> slots) noexcept
        {
            assert(slots[slot].qty > 0); // a 0-lot order never rests

            slots[slot].levelLinks.next_ = NULL_IDX;
            slots[slot].levelLinks.prev_ = tail;

            if (tail != NULL_IDX)
            {
                slots[tail].levelLinks.next_ = slot;
            }
            else
            {
                head = slot;
            }

            tail = slot;
            totalQty += slots[slot].qty;
            ++orderCount;
        }

        // Takes the order in `slot` out of the queue. Does not free the slot.
        void remove(uint32_t slot, std::span<PoolOrder> slots) noexcept
        {
            const PriceLevelLinks& links = slots[slot].levelLinks;

            // The order must be in this queue: its neighbours, or head and tail, point back at it.
            assert(
                links.prev_ != NULL_IDX ? slots[links.prev_].levelLinks.next_ == slot : head == slot
            );
            assert(
                links.next_ != NULL_IDX ? slots[links.next_].levelLinks.prev_ == slot : tail == slot
            );

            if (links.prev_ != NULL_IDX)
            {
                slots[links.prev_].levelLinks.next_ = links.next_;
            }
            else
            {
                head = links.next_;
            }

            if (links.next_ != NULL_IDX)
            {
                slots[links.next_].levelLinks.prev_ = links.prev_;
            }
            else
            {
                tail = links.prev_;
            }

            totalQty -= slots[slot].qty;
            --orderCount;
        }

        // Takes `by` off the order in `slot`.
        // Taking an order to zero must be a remove, not a reduce.
        void reduce(uint32_t slot, types::Quantity by, std::span<PoolOrder> slots) noexcept
        {
            slots[slot].qty -= by;
            totalQty -= by;
        }
    };

} // namespace engine::book