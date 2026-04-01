#pragma once
#include "ThreadPool.hpp"
#include "InstrumentContext.hpp"
#include "ArrayBitMapLocator.hpp"
#include <unordered_map>
#include <thread>
#include <memory>
#include <functional>
#include <atomic>

namespace engine
{

    class MatchingCore
    {
    public:
        struct Config
        {
            size_t numWorkers = std::max(1u,
                                         static_cast<unsigned>(std::thread::hardware_concurrency()) - 2);
            int firstWorkerCore = 2;
        };

        explicit MatchingCore(Config cfg = {});
        ~MatchingCore();

        MatchingCore(const MatchingCore &) = delete;
        MatchingCore &operator=(const MatchingCore &) = delete;

        void addInstrument(types::InstrumentId id, book::PriceRange range);

        void start();

        void stop();

        bool submit(engine::core::Command cmd) noexcept;

        types::Price bestBid(types::InstrumentId id) const noexcept;
        types::Price bestAsk(types::InstrumentId id) const noexcept;

        size_t instrumentCount() const noexcept { return contexts_.size(); }

    private:
        void drainerLoop() noexcept;

        Config cfg_;

        std::unordered_map<types::InstrumentId,
                           std::unique_ptr<InstrumentContext>>
            contexts_;

        std::unique_ptr<ThreadPool> pool_;
    };

}