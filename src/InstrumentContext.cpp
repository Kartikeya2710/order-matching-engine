#include "InstrumentContext.hpp"

namespace engine
{
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