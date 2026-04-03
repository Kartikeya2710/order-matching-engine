#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <iomanip>
#include "MatchingCore.hpp"
#include "InstrumentConfig.hpp"

static constexpr uint32_t TICK = 1;

static engine::core::Command makeLimit(
    engine::core::CommandType type,
    uint32_t orderId,
    uint32_t instrumentId,
    engine::types::Verb verb,
    uint32_t price,
    uint32_t qty,
    engine::types::TimeInForce tif = engine::types::TimeInForce::GTC)
{
    engine::core::Command cmd{};
    cmd.type = type;
    cmd.orderId = orderId;
    cmd.instrumentId = instrumentId;
    cmd.clientId = 1;
    cmd.verb = verb;
    cmd.orderType = engine::types::OrderType::Limit;
    cmd.tif = tif;
    cmd.limitPrice = price;
    cmd.qty = qty;
    return cmd;
}

static engine::core::Command makeMarket(
    uint32_t orderId, uint32_t instrumentId,
    engine::types::Verb verb, uint32_t qty)
{
    engine::core::Command cmd{};
    cmd.type = engine::core::CommandType::AddOrder;
    cmd.orderId = orderId;
    cmd.instrumentId = instrumentId;
    cmd.clientId = 1;
    cmd.verb = verb;
    cmd.orderType = engine::types::OrderType::Market;
    cmd.tif = engine::types::TimeInForce::IOC;
    cmd.limitPrice = engine::book::NO_PRICE;
    cmd.qty = qty;
    return cmd;
}

static void printEvent(const engine::core::TradeEvent &ev)
{
    using T = engine::core::TradeEvent::Type;
    switch (ev.type)
    {
    case T::OrderAccepted:
        std::cout << "  [ACCEPTED]  order=" << ev.aggressorOrderId
                  << " remaining=" << ev.aggressorRemaining << "\n";
        break;
    case T::OrderRejected:
        std::cout << "  [REJECTED]  order=" << ev.aggressorOrderId << "\n";
        break;
    case T::OrderCancelled:
        std::cout << "  [CANCELLED] order=" << ev.aggressorOrderId << "\n";
        break;
    case T::Fill:
        std::cout << "  [FILL]      aggressor=" << ev.aggressorOrderId
                  << " passive=" << ev.passiveOrderId
                  << " price=$" << std::fixed << std::setprecision(2)
                  << (ev.fillPrice / 100.0)
                  << " qty=" << ev.fillQty << "\n";
        break;
    case T::PartialFill:
        std::cout << "  [PARTIAL]   aggressor=" << ev.aggressorOrderId
                  << " passive=" << ev.passiveOrderId
                  << " price=$" << std::fixed << std::setprecision(2)
                  << (ev.fillPrice / 100.0)
                  << " qty=" << ev.fillQty
                  << " passive_remaining=" << ev.passiveRemaining << "\n";
        break;
    }
}

int main(int argc, char *argv[])
{
    const std::string configPath = (argc > 1) ? argv[1] : "instruments.cfg";

    // Load instrument config
    std::vector<engine::InstrumentConfig> instruments;
    try
    {
        instruments = engine::loadInstrumentConfig(configPath);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "Config error: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "Loaded " << instruments.size() << " instrument(s) from "
              << configPath << "\n";

    // Start the engine
    engine::MatchingCore::Config cfg;
    cfg.numWorkers = 2;
    cfg.firstWorkerCore = 2;

    engine::MatchingCore core(cfg);
    core.loadInstruments(instruments);

    std::atomic<int> eventCount{0};

    core.setTradeCallback([&](const engine::core::TradeEvent &ev)
                          {
        printEvent(ev);
        ++eventCount; });

    core.start();

    core.stop();
    return 0;
}