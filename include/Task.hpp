#pragma once
#include <coroutine>
#include <exception>

namespace engine
{
    struct Task
    {
        struct promise_type
        {
            Task get_return_object() noexcept
            {
                return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_never final_suspend() noexcept { return {}; }
            void return_void() noexcept {}
            void unhandled_exception() noexcept { std::terminate(); }
        };

        using handle_type = std::coroutine_handle<>;

        handle_type handle;

        ~Task()
        {
            if (handle && !handle.done())
            {
                handle.destroy();
            }
        }

        Task(const Task &) = delete;
        Task &operator=(const Task &) = delete;

        Task(Task &&other) noexcept : handle(other.handle)
        {
            other.handle = nullptr;
        }

        explicit Task(handle_type h) noexcept : handle(h) {}
    };
}