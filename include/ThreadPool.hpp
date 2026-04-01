#pragma once
#include "CoroutineWorker.hpp"
#include "InstrumentContext.hpp"
#include <cstddef>

namespace engine
{
    class ThreadPool
    {
    private:
        size_t numWorkers_;
        std::vector<std::unique_ptr<CoroutineWorker>> workers_;

    public:
        ThreadPool(size_t numWorkers, int firstCore);

        void assignInstrument(InstrumentContext *ctx) noexcept;

        void startAll();

        void stopAll();

        size_t workerCount() const noexcept;
    };
}