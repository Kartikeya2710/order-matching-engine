#include "Types.hpp"
#include <cstddef>

namespace engine::book
{

    struct alignas(64) PoolOrder
    {
        types::OrderId orderId;
        types::Verb verb;
        types::ClientId clientId;
        types::Price price;
        types::Quantity qty;
        types::TimeInForce tif;
        std::uint32_t next;
        std::uint32_t prev;
    };

    template <std::uint32_t Capacity>
    class OrderPool
    {
    public:
        static_assert(Capacity > 0 && Capacity <= (1u << 20), "Pool capacity must be between 1 and 1M orders");

        OrderPool() noexcept
        {
            for (std::uint32_t i = 0; i < Capacity; ++i)
            {
                freeStack_[i] = Capacity - i - 1;
            }
            freeTop_ = Capacity - 1;
        }

        [[nodiscard]] std::uint32_t acquire() noexcept
        {
            if (__builtin_expect(freeTop_ == -1, 0))
                return NULL_IDX;
            return freeStack_[freeTop_--];
        }

        void release(std::uint32_t idx) noexcept
        {
            freeStack_[++freeTop_] = idx;
        }

        PoolOrder &operator[](std::uint32_t idx) noexcept { return slots_[idx]; }
        const PoolOrder &operator[](std::uint32_t idx) const noexcept { return slots_[idx]; }
        std::uint32_t freeCount() const noexcept { return freeTop_; }

    private:
        alignas(64) PoolOrder slots_[Capacity];
        std::uint32_t freeStack_[Capacity];
        // the next index inside freeStack_ that is available
        std::uint32_t freeTop_;
    };

}
