#pragma once
#include "Task.hpp"
#include "InstrumentContext.hpp"

namespace engine
{
    struct QueueAwaitable
    {
        InstrumentContext *ctx;

        bool await_ready() const noexcept;

        bool await_suspend(std::coroutine_handle<> handle) noexcept;

        void await_resume() const noexcept;
    };
}