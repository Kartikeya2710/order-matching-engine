#include "ThreadPool.hpp"

namespace engine
{
    ThreadPool::ThreadPool(size_t numWorkers, int firstCore) : numWorkers_(numWorkers)
    {
        workers_.reserve(numWorkers);

        for (size_t i = 0; i < numWorkers; ++i)
        {
            workers_.emplace_back(std::make_unique<CoroutineWorker>((int)i, firstCore + (int)i));
        }
    }

    void ThreadPool::assignInstrument(InstrumentContext *ctx) noexcept
    {
        size_t workerIdx = ctx->instrumentId % numWorkers_;
        workers_[workerIdx]->assignInstrument(ctx);
    }

    void ThreadPool::startAll()
    {
        for (auto &w : workers_)
            w->start();
    }

    void ThreadPool::stopAll()
    {
        for (auto &w : workers_)
            w->stop();
    }

    size_t ThreadPool::workerCount() const noexcept { return numWorkers_; }
}