#include "CoroutineWorker.hpp"

namespace engine
{
    CoroutineWorker::CoroutineWorker(int workerId, int pinnedCore)
        : workerId_(workerId), pinnedCore_(pinnedCore) {}

    CoroutineWorker::~CoroutineWorker()
    {
        if (thread_.joinable())
            stop();
    }

    void CoroutineWorker::assignInstrument(InstrumentContext *ctx)
    {
        ctx->assignedWorker.store(workerId_, std::memory_order_relaxed);
        ctx->wakeQueue = &wakeQueue_; // tell gateway where to push wake-ups

        Task t = instrumentCoroutine(ctx);
        // Add to ready list so it runs on the first scheduler tick
        // (where it hits co_await, parks itself, and waits for real work).
        initialReady_.push_back(t.handle.address());
        tasks_.push_back(std::move(t));
    }

    void CoroutineWorker::start()
    {
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread(&CoroutineWorker::run, this);
        pinThreadToCore(thread_, pinnedCore_);
    }

    void CoroutineWorker::stop()
    {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable())
            thread_.join();
    }

    void CoroutineWorker::run() noexcept
    {
        setThreadName("coro-" + std::to_string(workerId_));
        std::vector<void *> ready = std::move(initialReady_);

        while (__builtin_expect(running_.load(std::memory_order_relaxed), 1))
        {
            {
                void *h;
                while (wakeQueue_.dequeue(h))
                    ready.push_back(h);
            }

            for (void *addr : ready)
                std::coroutine_handle<>::from_address(addr).resume();

            ready.clear();

            if (wakeQueue_.isEmpty())
                CPU_RELAX();
        }
    }

    Task instrumentCoroutine(InstrumentContext *ctx) noexcept
    {
        engine::core::Command cmd;
        while (true)
        {
            co_await QueueAwaitable{ctx};

            while (ctx->inputQueue.dequeue(cmd))
            {
                std::visit([&](auto &book) -> void
                           {
                    switch (cmd.type) {
                        case engine::core::CommandType::AddOrder:    
                        {
                            return book.addOrder(cmd);
                        }
                        case engine::core::CommandType::CancelOrder:
                        {
                            return book.cancelOrder(cmd);
                        }
                        case engine::core::CommandType::ModifyOrder:
                        {
                            return book.modifyOrder(cmd);
                        }
                        default:                       
                        {
                            return ;
                        }
                    } }, ctx->book);
            }
        }

        co_return;
    }
}