#pragma once
#include "Types.hpp"
#include <cstddef>
#include <cstdint>

namespace engine::book
{

    struct alignas(64) PoolOrder
    {
        types::OrderId orderId = 0;
        types::Verb verb = types::Verb::Buy;
        types::ClientId clientId = 0;
        types::Price price = 0;
        types::Quantity qty = 0;
        types::TimeInForce tif = types::TimeInForce::GTC;
        uint32_t next = UINT32_MAX;
        uint32_t prev = UINT32_MAX;
    };

    template <uint32_t Capacity>
    class OrderPool
    {
    public:
        static_assert(Capacity > 0 && Capacity <= (1u << 20),
                      "Pool capacity must be between 1 and 1M orders");

        OrderPool() noexcept
        {
            for (uint32_t i = 0; i < Capacity; ++i)
                freeStack_[i] = Capacity - i - 1;
            freeTop_ = static_cast<int32_t>(Capacity) - 1;
        }

        // Returns UINT32_MAX if pool is exhausted.
        [[nodiscard]] uint32_t acquire() noexcept
        {
            if (__builtin_expect(freeTop_ < 0, 0))
                return UINT32_MAX;
            return freeStack_[freeTop_--];
        }

        void release(uint32_t idx) noexcept
        {
            freeStack_[++freeTop_] = idx;
        }

        PoolOrder &operator[](uint32_t idx) noexcept { return slots_[idx]; }
        const PoolOrder &operator[](uint32_t idx) const noexcept { return slots_[idx]; }

        uint32_t freeCount() const noexcept
        {
            return static_cast<uint32_t>(freeTop_ + 1);
        }

    private:
        alignas(64) PoolOrder slots_[Capacity];
        uint32_t freeStack_[Capacity];
        int32_t freeTop_;
    };

}
