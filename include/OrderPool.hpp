#pragma once
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Types.hpp"

namespace engine::book
{
    // No slot: the end of a queue, an empty level, or an exhausted pool.
    static constexpr uint32_t NULL_IDX = UINT32_MAX;

    struct PriceLevel;

    template <uint32_t Capacity>
    class OrderPool;

    // An order's data. The book builds it and the pool stores it.
    struct Order
    {
        types::OrderId orderId = 0;
        types::Verb verb = types::Verb::Buy;
        types::ClientId clientId = 0;
        types::Price price = 0;
        types::Quantity qty = 0;
        types::TimeInForce tif = types::TimeInForce::GTC;
    };

    // One pool slot: an order plus its links in its price level's queue. Anyone can read it.
    // Only OrderPool writes it, when it acquires the slot, and only PriceLevel changes qty and the
    // links while the order rests.
    class alignas(64) PoolSlot
    {
    public:
        PoolSlot() = default;
        PoolSlot(const PoolSlot&) = default;

        // A slot is written only by OrderPool::acquire
        PoolSlot& operator=(const PoolSlot&) = delete;

        [[nodiscard]] const Order& order() const noexcept
        {
            return order_;
        }

        [[nodiscard]] uint32_t next() const noexcept
        {
            return next_;
        }

        [[nodiscard]] uint32_t prev() const noexcept
        {
            return prev_;
        }

    private:
        template <uint32_t Capacity>
        friend class OrderPool;
        friend struct PriceLevel;

        Order order_;
        uint32_t next_ = NULL_IDX;
        uint32_t prev_ = NULL_IDX;
    };

    template <uint32_t Capacity>
    class OrderPool
    {
    public:
        static_assert(
            Capacity > 0 && Capacity <= (1u << 20),
            "Pool capacity must be between 1 and 1M orders"
        );

        OrderPool() noexcept
        {
            for (uint32_t i = 0; i < Capacity; ++i)
            {
                freeStack_[i] = Capacity - i - 1;
            }
            freeTop_ = static_cast<int32_t>(Capacity) - 1;
        }

        // Takes a free slot and writes `order` into it. Returns `NULL_IDX` if the pool is
        // exhausted.
        [[nodiscard]] uint32_t acquire(const Order& order) noexcept
        {
            if (__builtin_expect(freeTop_ < 0, 0))
            {
                return NULL_IDX;
            }

            const uint32_t slotIdx = freeStack_[freeTop_--];

#ifndef NDEBUG
            acquired_[slotIdx] = true;
#endif

            PoolSlot& slot = slots_[slotIdx];
            slot.order_ = order;
            slot.next_ = NULL_IDX;
            slot.prev_ = NULL_IDX;

            return slotIdx;
        }

        void release(uint32_t slotIdx) noexcept
        {
#ifndef NDEBUG
            // Releasing a slot twice would cause two orders to share a slot
            // causing one order to override the other in the same slot
            assert(acquired_[slotIdx]);
            acquired_[slotIdx] = false;
#endif

            freeStack_[++freeTop_] = slotIdx;
        }

        const PoolSlot& operator[](uint32_t slotIdx) const noexcept
        {
            return slots_[slotIdx];
        }

        // All slots, for PriceLevel to change qty and the next/prev links between them.
        [[nodiscard]] std::span<PoolSlot> slots() noexcept
        {
            return slots_;
        }

        [[nodiscard]] uint32_t freeCount() const noexcept
        {
            return static_cast<uint32_t>(freeTop_ + 1);
        }

    private:
        alignas(64) std::array<PoolSlot, Capacity> slots_;
        std::array<uint32_t, Capacity> freeStack_;
        int32_t freeTop_;

#ifndef NDEBUG
        // For the assert in release()
        std::array<bool, Capacity> acquired_{};
#endif
    };

} // namespace engine::book
