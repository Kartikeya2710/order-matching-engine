#pragma once
#include "InstrumentContext.hpp"
#include <coroutine>

namespace engine
{
    struct QueueAwaitable
    {
        InstrumentContext* ctx;

        [[nodiscard]] bool await_ready() const noexcept;

        bool await_suspend(std::coroutine_handle<> handle) noexcept;

        void await_resume() const noexcept;
    };
} // namespace engine