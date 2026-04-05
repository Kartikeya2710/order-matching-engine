#pragma once
#include "OrderBook.hpp"
#include "Command.hpp"
#include "Threading.hpp"
#include "RingBuffer.hpp"
#include "InstrumentConfig.hpp"
#include "InstrumentContext.hpp"
#include <atomic>
#include <variant>
#include <coroutine>

namespace engine
{
    // I plan to add other variants in the future, maybe a FlexBook
    using BookVariant = std::variant<book::FastBook>;

    struct alignas(64) InstrumentContext
    {
        utils::SPSC_RingBuffer<core::Command, 4096> inputQueue;
        utils::SPSC_RingBuffer<core::TradeEvent, 1024> outputQueue;

        types::InstrumentId instrumentId = 0;
        BookVariant book;
        std::atomic<int> assignedWorker{-1};

        std::atomic<void *> pendingHandle{nullptr};
        utils::SPSC_RingBuffer<void *, 1024> *wakeQueue{nullptr};

        InstrumentContext(const InstrumentConfig &cfg)
            : instrumentId(cfg.instrumentId),
              book([&]() -> BookVariant
                   {
              switch (cfg.bookType) {
                  case BookType::FastBook:
                        return book::FastBook(book::ArrayBitMapLocator(cfg.priceRange), cfg.instrumentId);
                  default:
                      throw std::runtime_error("Unsupported BookType");
              } }())
        {
            std::visit([this](auto &b)
                       { b.setEventCallback([this](const core::TradeEvent &ev) noexcept
                                            {
                    core::TradeEvent copy = ev;
                    (void)this->outputQueue.enqueue(std::move(copy)); }); }, book);
        }

        InstrumentContext(const InstrumentContext &) = delete;
        InstrumentContext &operator=(const InstrumentContext &) = delete;
    };

    inline bool gatewayEnqueue(InstrumentContext *ctx,
                               core::Command cmd) noexcept
    {
        if (!ctx->inputQueue.enqueue(std::move(cmd)))
            return false; // queue full

        // Atomically grab the parked handle (and clear pendingHandle to null).
        void *h = ctx->pendingHandle.exchange(nullptr, std::memory_order_acq_rel);
        if (h != nullptr)
        {
            while (!ctx->wakeQueue->enqueue(std::move(h)))
                CPU_RELAX();
        }
        return true;
    }
}