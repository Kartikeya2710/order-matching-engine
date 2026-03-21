#pragma once
#include "OrderBook.hpp"
#include "Command.hpp"
#include "RingBuffer.hpp"

namespace engine
{
    // I plan to add other variants in the future, maybe a FlexBook
    using BookVariant = std::variant<book::FastBook>;

    struct alignas(64) InstrumentContext
    {
        utils::SPSC_RingBuffer<engine::core::Command, 4096> inputQueue;

        BookVariant book;

        types::InstrumentId instrumentId = 0;
        std::atomic<int> assignedWorker{-1};
        InstrumentContext(const InstrumentContext &) = delete;
        InstrumentContext &operator=(const InstrumentContext &) = delete;
    };
}