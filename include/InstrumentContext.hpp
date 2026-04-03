#pragma once
#include "OrderBook.hpp"
#include "Command.hpp"
#include "Threading.hpp"
#include "RingBuffer.hpp"
#include <atomic>
#include <variant>
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

        // Coroutine wake-up mechanism:
        //   null     = coroutine is running or not yet started
        //   non-null = coroutine is parked; gateway should push to wakeQueue
        std::atomic<void *> pendingHandle{nullptr};
        utils::SPSC_RingBuffer<void *, 1024> *wakeQueue{nullptr};

        InstrumentContext(const InstrumentContext &) = delete;
        InstrumentContext &operator=(const InstrumentContext &) = delete;
    };

    // Called by the gateway thread instead of ctx->inputQueue.enqueue() directly.
    // Enqueues the command and wakes the coroutine if it was sleeping.
    inline bool gatewayEnqueue(InstrumentContext *ctx,
                               core::Command cmd) noexcept
    {
        if (!ctx->inputQueue.enqueue(std::move(cmd)))
            return false; // queue full

        // Atomically grab the parked handle (and clear pendingHandle to null).
        void *h = ctx->pendingHandle.exchange(nullptr, std::memory_order_acq_rel);
        if (h != nullptr)
        {
            while (!ctx->wakeQueue->enqueue(std::move(h)))
                CPU_RELAX();
        }
        return true;
    }
}
