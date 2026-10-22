#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "Types.hpp"

namespace engine::book
{
    // No slot: the end of a queue, an empty level, or an exhausted pool.
    static constexpr uint32_t NULL_IDX = UINT32_MAX;

    struct PriceLevel;

    // An order's place in its price level's queue. Anyone can read it to walk the queue; only
    // PriceLevel can change it.
    class PriceLevelLinks
    {
    public:
        [[nodiscard]] uint32_t next() const noexcept
        {
            return next_;
        }

        [[nodiscard]] uint32_t prev() const noexcept
        {
            return prev_;
        }

    private:
        friend struct PriceLevel;

        uint32_t next_ = NULL_IDX;
        uint32_t prev_ = NULL_IDX;
    };

    struct alignas(64) PoolOrder
    {
        types::OrderId orderId = 0;
        types::Verb verb = types::Verb::Buy;
        types::ClientId clientId = 0;
        types::Price price = 0;
        types::Quantity qty = 0;
        types::TimeInForce tif = types::TimeInForce::GTC;
        PriceLevelLinks levelLinks;
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

        // Returns NULL_IDX if pool is exhausted.
        [[nodiscard]] uint32_t acquire() noexcept
        {
            if (__builtin_expect(freeTop_ < 0, 0))
            {
                return NULL_IDX;
            }

            return freeStack_[freeTop_--];
        }

        void release(uint32_t idx) noexcept
        {
            freeStack_[++freeTop_] = idx;
        }

        PoolOrder& operator[](uint32_t idx) noexcept
        {
            return slots_[idx];
        }

        const PoolOrder& operator[](uint32_t idx) const noexcept
        {
            return slots_[idx];
        }

        // All slots, for code that follows the next/prev links between them.
        [[nodiscard]] std::span<PoolOrder> slots() noexcept
        {
            return slots_;
        }

        [[nodiscard]] uint32_t freeCount() const noexcept
        {
            return static_cast<uint32_t>(freeTop_ + 1);
        }

    private:
        alignas(64) std::array<PoolOrder, Capacity> slots_;
        std::array<uint32_t, Capacity> freeStack_;
        int32_t freeTop_;
    };

} // namespace engine::book
