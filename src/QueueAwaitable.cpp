#include "QueueAwaitable.hpp"

namespace engine
{
    bool QueueAwaitable::await_ready() const noexcept
    {
        return !ctx->inputQueue.isEmpty();
    }

    bool QueueAwaitable::await_suspend(std::coroutine_handle<> handle) noexcept
    {
        ctx->pendingHandle.store(handle.address(), std::memory_order_release);

        // Re-check: between await_ready() and this store, the gateway might
        // have enqueued something. If so, clear the handle and don't suspend
        // (otherwise we'd sleep forever — nobody will wake us).
        if (!ctx->inputQueue.isEmpty())
        {
            ctx->pendingHandle.store(nullptr, std::memory_order_release);
            return false;
        }

        return true;
    }

    void QueueAwaitable::await_resume() const noexcept {}
}