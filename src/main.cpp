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

    core.start();

    core.stop();
    return 0;
}