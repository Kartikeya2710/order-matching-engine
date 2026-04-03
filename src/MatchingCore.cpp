#include "MatchingCore.hpp"

namespace engine
{

    MatchingCore::MatchingCore(Config cfg)
        : cfg_(cfg), pool_(std::make_unique<ThreadPool>(cfg.numWorkers, cfg.firstWorkerCore))
    {
    }

    MatchingCore::~MatchingCore()
    {
        stop();
    }

    void MatchingCore::loadInstruments(const std::vector<InstrumentConfig> &configs)
    {
        for (const auto &cfg : configs)
        {
            addInstrument(cfg);
        }
    }

    void MatchingCore::setTradeCallback(TradeCallback cb)
    {
        tradeCallback_ = std::move(cb);
    }

    void MatchingCore::addInstrument(InstrumentConfig cfg)
    {
        types::InstrumentId id = cfg.instrumentId;

        auto ctx = std::make_unique<InstrumentContext>(cfg);

        pool_->assignInstrument(ctx.get());
        contexts_[id] = std::move(ctx);
    }

    void MatchingCore::start()
    {
        drainerRunning_.store(true, std::memory_order_relaxed);
        drainerThread_ = std::thread(&MatchingCore::drainerLoop, this);
        pool_->startAll();
    }

    void MatchingCore::stop()
    {
        pool_->stopAll();
        drainerRunning_.store(false, std::memory_order_release);
        if (drainerThread_.joinable())
            drainerThread_.join();
    }

    bool MatchingCore::submit(engine::core::Command cmd) noexcept
    {
        auto it = contexts_.find(cmd.instrumentId);
        if (__builtin_expect(it == contexts_.end(), 0))
            return false; // unknown instrument

        return gatewayEnqueue(it->second.get(), std::move(cmd));
    }

    types::Price MatchingCore::bestBid(types::InstrumentId id) const noexcept
    {
        auto it = contexts_.find(id);
        if (it == contexts_.end())
            return book::NO_PRICE;
        return std::visit([](const auto &book)
                          { return book.bestBid(); },
                          it->second->book);
    }

    types::Price MatchingCore::bestAsk(types::InstrumentId id) const noexcept
    {
        auto it = contexts_.find(id);
        if (it == contexts_.end())
            return book::NO_PRICE;
        return std::visit([](const auto &book)
                          { return book.bestAsk(); },
                          it->second->book);
    }

    void MatchingCore::drainerLoop() noexcept
    {
        engine::core::TradeEvent ev;

        while (drainerRunning_.load(std::memory_order_relaxed))
        {
            bool anyWork = false;
            for (auto &[id, ctx] : contexts_)
            {
                while (ctx->outputQueue.dequeue(ev))
                {
                    anyWork = true;
                    if (tradeCallback_)
                        tradeCallback_(ev);
                }
            }
            if (!anyWork)
                CPU_RELAX();
        }

        for (auto &[id, ctx] : contexts_)
        {
            while (ctx->outputQueue.dequeue(ev))
            {
                if (tradeCallback_)
                    tradeCallback_(ev);
            }
        }
    }
}