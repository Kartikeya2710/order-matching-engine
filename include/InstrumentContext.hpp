#pragma once
#include "OrderBook.hpp"
#include "Command.hpp"
#include "RingBuffer.hpp"
#include <atomic>
#include <immintrin.h>
#include <coroutine>

namespace engine
{
    // I plan to add other variants in the future, maybe a FlexBook
    using BookVariant = std::variant<book::FastBook>;

    struct alignas(64) InstrumentContext
    {
        utils::SPSC_RingBuffer<engine::core::Command, 4096> inputQueue;

        BookVariant book;
        types::InstrumentId instrumentId = 0;
        std::atomic<int> assignedWorker{-1};

        std::atomic<void *> pendingHandle{nullptr};
        utils::SPSC_RingBuffer<void *, 1024> *wakeQueue{nullptr};

        InstrumentContext(const InstrumentContext &) = delete;
        InstrumentContext &operator=(const InstrumentContext &) = delete;
    };

    inline bool gatewayEnqueue(InstrumentContext *ctx, engine::core::Command &cmd) noexcept
    {
        if (!ctx->inputQueue.enqueue(std::move(cmd)))
            return false; // queue full

        void *h = ctx->pendingHandle.exchange(nullptr, std::memory_order_acq_rel);
        if (h != nullptr)
        {
            while (!ctx->wakeQueue->enqueue(std::move(h)))
                _mm_pause();
        }
        return true;
    }
}
