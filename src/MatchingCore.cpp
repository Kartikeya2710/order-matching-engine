#include "MatchingCore.hpp"
#include <immintrin.h>
#include <variant>

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

    void MatchingCore::addInstrument(types::InstrumentId id,
                                     book::PriceRange range)
    {
        auto ctx = std::make_unique<InstrumentContext>();
        ctx->instrumentId = id;

        InstrumentContext *rawCtx = ctx.get();

        // in-place construction of the book inside the variant's memory
        ctx->book.emplace<book::FastBook>(
            book::ArrayBitMapLocator(range),
            id);

        pool_->assignInstrument(ctx.get());
        contexts_[id] = std::move(ctx);
    }

    void MatchingCore::start()
    {
        pool_->startAll();
    }

    void MatchingCore::stop()
    {
        pool_->stopAll();
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

}