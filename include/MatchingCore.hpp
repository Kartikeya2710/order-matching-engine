#pragma once
#include "ThreadPool.hpp"
#include "InstrumentContext.hpp"
#include "ArrayBitMapLocator.hpp"
#include "InstrumentConfig.hpp"
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
            size_t numWorkers;
            int firstWorkerCore;

            Config() : numWorkers(std::max(1u, std::thread::hardware_concurrency() - 2)),
                       firstWorkerCore(2) {}
        };

        explicit MatchingCore(Config cfg = {});
        ~MatchingCore();

        MatchingCore(const MatchingCore &) = delete;
        MatchingCore &operator=(const MatchingCore &) = delete;

        void loadInstruments(const std::vector<InstrumentConfig> &configs);

        void addInstrument(InstrumentConfig cfg);

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