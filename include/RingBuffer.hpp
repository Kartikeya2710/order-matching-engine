#pragma once
#include <array>
#include <new>
#include <atomic>
#include <bit>

namespace engine::utils
{

    // Leave one empty slot in the buffer to differentiate between full and empty condition (otherwise, read_idx == write_idx in both the cases).
    // This helps us avoid having to maintain a state indicating whether the buffer is full or empty.
    template <typename data_T, size_t size_T>
    class SPSC_RingBuffer
    {
    private:
        struct cell_t
        {
            alignas(data_T) std::byte m_cell[sizeof(data_T)];
        };

        static constexpr std::size_t mask = size_T - 1;
        alignas(64) std::atomic<std::size_t> read_idx;
        alignas(64) std::atomic<std::size_t> write_idx;
        alignas(64) std::array<cell_t, size_T> buffer;

    public:
        static_assert(size_T >= 2, "Queue size must be at least 2");
        static_assert(std::has_single_bit(size_T), "Queue size must be a power of 2");

        SPSC_RingBuffer() : read_idx(0), write_idx(0) {}

        ~SPSC_RingBuffer()
        {
            data_T tmp;
            while (dequeue(tmp))
            {
            }
        }

        auto enqueue(data_T &&element) -> bool
        {
            const auto current_write = write_idx.load(std::memory_order_relaxed);
            const auto next_write = (current_write + 1) & mask;

            // buffer is full
            if (next_write == read_idx.load(std::memory_order_acquire))
            {
                return false;
            }

            new (buffer[current_write].m_cell) data_T(std::move(element));

            write_idx.store(next_write, std::memory_order_release);
            return true;
        }

        auto dequeue(data_T &element_out) -> bool
        {
            const auto current_read = read_idx.load(std::memory_order_relaxed);

            // buffer is empty
            if (current_read == write_idx.load(std::memory_order_acquire))
            {
                return false;
            }

            data_T *element_ptr = reinterpret_cast<data_T *>(&buffer[current_read].m_cell);
            element_out = std::move(*element_ptr);
            element_ptr->~data_T();

            read_idx.store((current_read + 1) & mask, std::memory_order_release);
            return true;
        }

        bool isEmpty() noexcept
        {
            const auto current_write = write_idx.load(std::memory_order_relaxed);
            const auto current_read = read_idx.load(std::memory_order_relaxed);
            return current_write == current_read;
        }
    };

}
