#pragma once
#include "QueueAwaitable.hpp"
#include "InstrumentContext.hpp"
#include "Threading.hpp"
#include "RingBuffer.hpp"
#include "Task.hpp"
#include <thread>
#include <vector>
#include <atomic>

namespace engine
{
    class CoroutineWorker
    {
    public:
        explicit CoroutineWorker(int workerId, int pinnedCore);

        CoroutineWorker(const CoroutineWorker &) = delete;
        CoroutineWorker &operator=(const CoroutineWorker &) = delete;
        ~CoroutineWorker();

        void assignInstrument(InstrumentContext *ctx);

        void start();

        void stop();

    private:
        void run() noexcept;

        int workerId_;
        int pinnedCore_;

        utils::SPSC_RingBuffer<void *, 1024> wakeQueue_;

        std::vector<Task> tasks_;
        std::vector<void *> initialReady_; // handles to run on first tick

        std::thread thread_;
        alignas(64) std::atomic<bool> running_{false};
    };

    Task instrumentCoroutine(InstrumentContext *ctx) noexcept;
}