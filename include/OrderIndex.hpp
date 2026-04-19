#pragma once
#include <array>
#include <cstdint>
#include <cassert>

namespace engine::book
{
    // Fixed-capacity open-addressing hash map: uint64_t key -> uint32_t value.
    //
    // Designed for matching engine order lookup where:
    //   - Maximum live entries is known at compile time (PoolCap)
    //   - Keys are unstructured uint64_t (no monotonicity assumed)
    //   - Erase is frequent (~30-40% of traffic in equity markets)
    //
    // Cap must be a power of two and at least 2x the max live entry count.
    // At load <= 0.5, linear probing averages < 1.5 probes per operation.
    //
    // Deliberately has no dynamic allocation. The entire table is a flat
    // std::array — suitable for stack, BSS, or embedding inside another struct.
    template <size_t Cap>
    class OrderIndex
    {
        static_assert((Cap & (Cap - 1)) == 0, "Cap must be a power of two");

        static constexpr uint64_t EMPTY_KEY = UINT64_MAX;
        static constexpr size_t MASK = Cap - 1;

        alignas(16) struct Slot
        {
            uint64_t key = EMPTY_KEY;
            uint32_t value = 0;
        };

        std::array<Slot, Cap> slots_{};

        // MurmurHash3 finalizer — distributes unstructured uint64_t keys well.
        [[nodiscard]] static size_t hash(uint64_t k) noexcept
        {
            k ^= k >> 33;
            k *= 0xff51afd7ed558ccdULL;
            k ^= k >> 33;
            k *= 0xc4ceb9fe1a85ec53ULL;
            k ^= k >> 33;
            return static_cast<size_t>(k & MASK);
        }

    public:
        [[nodiscard]] uint32_t *find(uint64_t key) noexcept
        {
            assert(key != EMPTY_KEY && "UINT64_MAX is reserved as empty sentinel");
            for (size_t i = hash(key);; i = (i + 1) & MASK)
            {
                if (slots_[i].key == key)
                    return &slots_[i].value;
                if (slots_[i].key == EMPTY_KEY)
                    return nullptr;
            }
        }

        [[nodiscard]] const uint32_t *find(uint64_t key) const noexcept
        {
            assert(key != EMPTY_KEY && "UINT64_MAX is reserved as empty sentinel");
            for (size_t i = hash(key);; i = (i + 1) & MASK)
            {
                if (slots_[i].key == key)
                    return &slots_[i].value;
                if (slots_[i].key == EMPTY_KEY)
                    return nullptr;
            }
        }

        void insert(uint64_t key, uint32_t value) noexcept
        {
            assert(key != EMPTY_KEY && "UINT64_MAX is reserved as empty sentinel");
            size_t i = hash(key);
            while (slots_[i].key != EMPTY_KEY)
                i = (i + 1) & MASK;
            slots_[i] = {key, value, 0};
        }

        void erase(uint64_t key) noexcept
        {
            assert(key != EMPTY_KEY && "UINT64_MAX is reserved as empty sentinel");

            size_t i = hash(key);
            while (slots_[i].key != key)
            {
                if (slots_[i].key == EMPTY_KEY)
                    return;
                i = (i + 1) & MASK;
            }

            size_t hole = i;
            for (;;)
            {
                size_t j = (hole + 1) & MASK;

                if (slots_[j].key == EMPTY_KEY)
                {
                    slots_[hole].key = EMPTY_KEY;
                    return;
                }

                // Is entry at j displaced past hole? i.e. does its home position
                // fall outside the range (hole, j], accounting for wraparound?
                size_t home = hash(slots_[j].key);
                bool displaced = (hole < j)
                                     ? (home <= hole || home > j)
                                     : (home <= hole && home > j);

                if (displaced)
                {
                    slots_[hole] = slots_[j];
                    slots_[j].key = EMPTY_KEY;
                    hole = j;
                }
                else
                {
                    hole = j;
                }
            }
        }
    };

}