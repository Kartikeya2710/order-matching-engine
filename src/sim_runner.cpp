#include "MatchingCore.hpp"
#include "InstrumentConfig.hpp"
#include "TradeEvent.hpp"
#include "Command.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

// ─── One row from the commands CSV ──────────────────────────────────────────
struct SimRow
{
    uint64_t timestamp_ns;
    engine::core::Command cmd;
};

// ─── Per-command latency/event record ───────────────────────────────────────
struct CmdResult
{
    uint64_t seq;
    uint64_t scheduled_ns;
    uint64_t submit_wall_ns;    // wall time when submit() called
    uint64_t submit_latency_ns; // duration of submit() call
    uint64_t first_event_ns;    // wall time of first callback event
    uint64_t last_event_ns;     // wall time of last callback event
    uint64_t e2e_latency_ns;    // submit() start → last event
    engine::core::Command cmd;
    std::vector<engine::core::TradeEvent> events;
};

struct SimConfig
{
    std::string commands_file = "commands.csv";
    std::string instruments_file = "instruments.cfg";
    std::string output_file = "sim_results.json";
    bool realtime = false;
    double speed = 1.0;
    uint32_t snapshot_ms = 100;
    uint32_t depth_levels = 10;
    uint32_t watch_instrument = 0;
    uint32_t num_workers = 2;
    int first_core = 2;
    bool tui = false;
};

// ─── CSV parse helpers ──────────────────────────────────────────────────────
static engine::core::CommandType parseCmdType(const std::string &s)
{
    if (s == "Add")
        return engine::core::CommandType::AddOrder;
    if (s == "Cancel")
        return engine::core::CommandType::CancelOrder;
    if (s == "Modify")
        return engine::core::CommandType::ModifyOrder;
    throw std::runtime_error("bad cmd type: " + s);
}
static engine::types::Verb parseVerb(const std::string &s)
{
    if (s == "Buy")
        return engine::types::Verb::Buy;
    if (s == "Sell")
        return engine::types::Verb::Sell;
    throw std::runtime_error("bad verb: " + s);
}
static engine::types::OrderType parseOT(const std::string &s)
{
    if (s == "Market")
        return engine::types::OrderType::Market;
    if (s == "Limit")
        return engine::types::OrderType::Limit;
    if (s == "Stop")
        return engine::types::OrderType::Stop;
    throw std::runtime_error("bad order type: " + s);
}
static engine::types::TimeInForce parseTIF(const std::string &s)
{
    if (s == "FOK")
        return engine::types::TimeInForce::FOK;
    if (s == "IOC")
        return engine::types::TimeInForce::IOC;
    if (s == "GTC")
        return engine::types::TimeInForce::GTC;
    if (s == "None")
        return engine::types::TimeInForce::None;
    throw std::runtime_error("bad tif: " + s);
}

std::vector<SimRow> loadCommands(const std::string &path)
{
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("cannot open " + path);
    std::vector<SimRow> rows;
    std::string line;
    int ln = 0;
    while (std::getline(f, line))
    {
        ++ln;
        if (line.empty() || line[0] == '#')
            continue;
        if (!line.empty() && (line[0] == 't' || line[0] == 'T'))
            continue; // header
        std::vector<std::string> tok;
        std::stringstream ss(line);
        std::string s;
        while (std::getline(ss, s, ','))
        {
            size_t a = s.find_first_not_of(" \t\r");
            size_t b = s.find_last_not_of(" \t\r");
            tok.push_back(a == std::string::npos ? "" : s.substr(a, b - a + 1));
        }
        if (tok.size() < 11)
            continue;
        try
        {
            SimRow r{};
            r.timestamp_ns = std::stoull(tok[0]);
            r.cmd.type = parseCmdType(tok[1]);
            r.cmd.orderId = static_cast<uint32_t>(std::stoul(tok[2]));
            r.cmd.instrumentId = static_cast<uint32_t>(std::stoul(tok[3]));
            r.cmd.clientId = static_cast<uint32_t>(std::stoul(tok[4]));
            r.cmd.tif = parseTIF(tok[5]);
            r.cmd.orderType = parseOT(tok[6]);
            r.cmd.verb = parseVerb(tok[7]);
            r.cmd.limitPrice = static_cast<uint32_t>(std::stoul(tok[8]));
            r.cmd.stopPrice = static_cast<uint32_t>(std::stoul(tok[9]));
            r.cmd.qty = static_cast<uint32_t>(std::stoul(tok[10]));
            rows.push_back(r);
        }
        catch (const std::exception &e)
        {
            std::cerr << "line " << ln << " skipped: " << e.what() << "\n";
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const SimRow &a, const SimRow &b)
              { return a.timestamp_ns < b.timestamp_ns; });
    return rows;
}

// ─── Percentile/stats helpers ───────────────────────────────────────────────
struct Stats
{
    double mean = 0, stddev = 0;
    double p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0;
    uint64_t min_v = 0, max_v = 0, count = 0;
};

Stats compute(std::vector<uint64_t> &v)
{
    Stats s{};
    if (v.empty())
        return s;
    std::sort(v.begin(), v.end());
    s.count = v.size();
    s.min_v = v.front();
    s.max_v = v.back();
    double sum = 0;
    for (auto x : v)
        sum += x;
    s.mean = sum / s.count;
    double var = 0;
    for (auto x : v)
        var += (x - s.mean) * (x - s.mean);
    s.stddev = std::sqrt(var / s.count);
    auto pct = [&](double p)
    {
        double i = p * (s.count - 1);
        size_t lo = (size_t)i;
        size_t hi = std::min(static_cast<unsigned>(lo) + 1, static_cast<unsigned>(s.count) - 1);
        double f = i - lo;
        return v[lo] * (1.0 - f) + v[hi] * f;
    };
    s.p50 = pct(0.5);
    s.p90 = pct(0.9);
    s.p95 = pct(0.95);
    s.p99 = pct(0.99);
    s.p999 = pct(0.999);
    return s;
}

// ─── Name helpers for output ────────────────────────────────────────────────
static const char *cmdName(engine::core::CommandType t)
{
    switch (t)
    {
    case engine::core::CommandType::AddOrder:
        return "Add";
    case engine::core::CommandType::CancelOrder:
        return "Cancel";
    case engine::core::CommandType::ModifyOrder:
        return "Modify";
    }
    return "?";
}
static const char *verbName(engine::types::Verb v) { return v == engine::types::Verb::Buy ? "Buy" : "Sell"; }
static const char *otName(engine::types::OrderType t)
{
    switch (t)
    {
    case engine::types::OrderType::Market:
        return "Market";
    case engine::types::OrderType::Limit:
        return "Limit";
    case engine::types::OrderType::Stop:
        return "Stop";
    }
    return "?";
}
static const char *tifName(engine::types::TimeInForce t)
{
    switch (t)
    {
    case engine::types::TimeInForce::FOK:
        return "FOK";
    case engine::types::TimeInForce::IOC:
        return "IOC";
    case engine::types::TimeInForce::GTC:
        return "GTC";
    case engine::types::TimeInForce::None:
        return "None";
    }
    return "?";
}
static const char *evName(engine::core::TradeEvent::Type t)
{
    using T = engine::core::TradeEvent::Type;
    switch (t)
    {
    case T::OrderAccepted:
        return "Accepted";
    case T::OrderRejected:
        return "Rejected";
    case T::OrderCancelled:
        return "Cancelled";
    case T::Fill:
        return "Fill";
    case T::PartialFill:
        return "PartialFill";
    }
    return "?";
}

// ─── Minimal ANSI TUI dashboard ─────────────────────────────────────────────
namespace tui
{
    constexpr const char *CLEAR = "\033[2J\033[H";
    constexpr const char *RESET = "\033[0m";
    constexpr const char *DIM = "\033[2m";
    constexpr const char *BOLD = "\033[1m";
    constexpr const char *GREEN = "\033[32m";
    constexpr const char *RED = "\033[31m";
    constexpr const char *YELLOW = "\033[33m";
    constexpr const char *BLUE = "\033[34m";
    constexpr const char *CYAN = "\033[36m";
    constexpr const char *GRAY = "\033[90m";
    constexpr const char *HIDE_CURSOR = "\033[?25l";
    constexpr const char *SHOW_CURSOR = "\033[?25h";

    std::string bar(double frac, int width, const char *color)
    {
        int filled = (int)(frac * width + 0.5);
        std::string s = color;
        for (int i = 0; i < filled; ++i)
            s += "█";
        s += GRAY;
        for (int i = filled; i < width; ++i)
            s += "░";
        s += RESET;
        return s;
    }

    std::string fmtNs(uint64_t n)
    {
        char buf[32];
        if (n >= 1'000'000)
            std::snprintf(buf, 32, "%.1fms", n / 1e6);
        else if (n >= 1'000)
            std::snprintf(buf, 32, "%.1fus", n / 1e3);
        else
            std::snprintf(buf, 32, "%lluns", (unsigned long long)n);
        return buf;
    }
}

// ─── Main ───────────────────────────────────────────────────────────────────
void usage(const char *prog)
{
    std::cout << "Usage: " << prog << " [options]\n"
                                      "  --commands FILE        CSV commands file (default: commands.csv)\n"
                                      "  --instruments FILE     Instruments config (default: instruments.cfg)\n"
                                      "  --output FILE          Output JSON (default: sim_results.json)\n"
                                      "  --burst                Feed commands as fast as possible (default)\n"
                                      "  --realtime             Respect timestamps\n"
                                      "  --speed N              Playback speed multiplier (default: 1.0)\n"
                                      "  --snapshot-ms N        Book snapshot interval in ms (0=off, default: 100)\n"
                                      "  --depth N              Depth levels per book snapshot (default: 10)\n"
                                      "  --watch ID             Only snapshot this instrument (0=all, default: 0)\n"
                                      "  --workers N            Worker threads (default: 2)\n"
                                      "  --first-core N         First core to pin to (default: 2)\n"
                                      "  --tui                  Live TUI dashboard\n"
                                      "  --help                 Show this help\n";
}

int main(int argc, char *argv[])
{
    SimConfig cfg;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto need = [&](const char *n)
        { if (i + 1 >= argc) { std::cerr << n << " needs arg\n"; std::exit(1); } return argv[++i]; };
        if (a == "--commands")
            cfg.commands_file = need("--commands");
        else if (a == "--instruments")
            cfg.instruments_file = need("--instruments");
        else if (a == "--output")
            cfg.output_file = need("--output");
        else if (a == "--realtime")
            cfg.realtime = true;
        else if (a == "--burst")
            cfg.realtime = false;
        else if (a == "--speed")
            cfg.speed = std::stod(need("--speed"));
        else if (a == "--snapshot-ms")
            cfg.snapshot_ms = (uint32_t)std::stoul(need("--snapshot-ms"));
        else if (a == "--depth")
            cfg.depth_levels = (uint32_t)std::stoul(need("--depth"));
        else if (a == "--watch")
            cfg.watch_instrument = (uint32_t)std::stoul(need("--watch"));
        else if (a == "--workers")
            cfg.num_workers = (uint32_t)std::stoul(need("--workers"));
        else if (a == "--first-core")
            cfg.first_core = std::stoi(need("--first-core"));
        else if (a == "--tui")
            cfg.tui = true;
        else if (a == "--help")
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "unknown: " << a << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    // ── Load config ─────────────────────────────────────────────────────
    std::vector<engine::InstrumentConfig> instruments;
    try
    {
        instruments = engine::loadInstrumentConfig(cfg.instruments_file);
    }
    catch (const std::exception &e)
    {
        std::cerr << "instruments: " << e.what() << "\n";
        return 1;
    }
    std::cout << "[SIM] loaded " << instruments.size() << " instrument(s)\n";

    std::vector<SimRow> rows;
    try
    {
        rows = loadCommands(cfg.commands_file);
    }
    catch (const std::exception &e)
    {
        std::cerr << "commands: " << e.what() << "\n";
        return 1;
    }
    std::cout << "[SIM] loaded " << rows.size() << " command(s)\n";
    if (rows.empty())
        return 1;

    // ── Engine setup ────────────────────────────────────────────────────
    engine::MatchingCore::Config coreCfg;
    coreCfg.numWorkers = cfg.num_workers;
    coreCfg.firstWorkerCore = cfg.first_core;
    engine::MatchingCore core(coreCfg);
    core.loadInstruments(instruments);

    // ── Metrics collection ──────────────────────────────────────────────
    std::vector<CmdResult> results(rows.size());
    std::vector<uint64_t> submit_lat;
    submit_lat.reserve(rows.size());

    // Event counters (atomic so the callback can update from any thread)
    std::atomic<uint64_t> ev_fill{0}, ev_partial{0}, ev_accept{0}, ev_reject{0}, ev_cancel{0};
    std::atomic<uint64_t> ev_volume{0}, ev_turnover{0};
    std::atomic<uint64_t> ev_total{0};

    // Map aggressorOrderId → results[idx] for correlation. Built once.
    std::unordered_map<uint32_t, size_t> oid_to_idx;
    oid_to_idx.reserve(rows.size() * 2);

    // Events collected from the callback (timestamped). Protected by mutex.
    std::mutex ev_mu;
    std::vector<std::pair<uint64_t, engine::core::TradeEvent>> raw_events;
    raw_events.reserve(rows.size() * 2);

    Clock::time_point sim_t0; // set just before starting

    core.setTradeCallback([&](const engine::core::TradeEvent &ev)
                          {
        auto wall = std::chrono::duration_cast<ns>(Clock::now() - sim_t0).count();
        switch (ev.type) {
            case engine::core::TradeEvent::Type::Fill:
                ev_fill.fetch_add(1, std::memory_order_relaxed);
                ev_volume.fetch_add(ev.fillQty, std::memory_order_relaxed);
                ev_turnover.fetch_add((uint64_t)ev.fillPrice * ev.fillQty, std::memory_order_relaxed);
                break;
            case engine::core::TradeEvent::Type::PartialFill:
                ev_partial.fetch_add(1, std::memory_order_relaxed);
                ev_volume.fetch_add(ev.fillQty, std::memory_order_relaxed);
                ev_turnover.fetch_add((uint64_t)ev.fillPrice * ev.fillQty, std::memory_order_relaxed);
                break;
            case engine::core::TradeEvent::Type::OrderAccepted:
                ev_accept.fetch_add(1, std::memory_order_relaxed); break;
            case engine::core::TradeEvent::Type::OrderRejected:
                ev_reject.fetch_add(1, std::memory_order_relaxed); break;
            case engine::core::TradeEvent::Type::OrderCancelled:
                ev_cancel.fetch_add(1, std::memory_order_relaxed); break;
        }
        ev_total.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(ev_mu);
        raw_events.emplace_back((uint64_t)wall, ev); });

    core.start();
    std::cout << "[SIM] engine started (" << cfg.num_workers << " workers, "
              << (cfg.realtime ? "REALTIME" : "BURST") << " mode)\n";

    // ── Book snapshot storage ───────────────────────────────────────────
    struct Snap
    {
        uint64_t wall_ns;
        uint32_t instrument_id;
        uint32_t best_bid;
        uint32_t best_ask;
        uint32_t spread;
    };
    std::vector<Snap> snaps;

    // ── Feed commands ───────────────────────────────────────────────────
    sim_t0 = Clock::now();
    uint64_t last_snap_ns = 0;
    uint64_t snap_int_ns = (uint64_t)cfg.snapshot_ms * 1'000'000ULL;
    auto last_tui_update = Clock::now();

    if (cfg.tui)
        std::cout << tui::HIDE_CURSOR << tui::CLEAR;

    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto &row = rows[i];

        // Realtime: wait until scheduled time
        if (cfg.realtime && row.timestamp_ns > 0)
        {
            uint64_t target_ns = (uint64_t)(row.timestamp_ns / cfg.speed);
            auto target = sim_t0 + ns(target_ns);
            if (Clock::now() < target)
            {
                if (target - Clock::now() > std::chrono::microseconds(100))
                    std::this_thread::sleep_until(target - std::chrono::microseconds(20));
                while (Clock::now() < target)
                { /* spin */
                }
            }
        }

        // Submit
        auto t0 = Clock::now();
        bool ok = core.submit(row.cmd);
        auto t1 = Clock::now();
        uint64_t sub_ns = std::chrono::duration_cast<ns>(t1 - t0).count();
        uint64_t wall_ns = std::chrono::duration_cast<ns>(t0 - sim_t0).count();

        auto &r = results[i];
        r.seq = i;
        r.scheduled_ns = row.timestamp_ns;
        r.submit_wall_ns = wall_ns;
        r.submit_latency_ns = sub_ns;
        r.cmd = row.cmd;
        submit_lat.push_back(sub_ns);

        // Build order ID → result index map (last write wins for ids reused
        // after cancel). This is fine since events arrive in causal order
        // per instrument.
        oid_to_idx[row.cmd.orderId] = i;

        if (!ok)
            std::cerr << "[SIM] submit failed cmd=" << i << " oid=" << row.cmd.orderId << "\n";

        // Book snapshots (polled by this thread between submits)
        if (snap_int_ns > 0 && (wall_ns - last_snap_ns) >= snap_int_ns)
        {
            last_snap_ns = wall_ns;
            for (const auto &inst : instruments)
            {
                if (cfg.watch_instrument != 0 && inst.instrumentId != cfg.watch_instrument)
                    continue;
                Snap s{};
                s.wall_ns = wall_ns;
                s.instrument_id = inst.instrumentId;
                s.best_bid = core.bestBid(inst.instrumentId);
                s.best_ask = core.bestAsk(inst.instrumentId);
                s.spread = (s.best_bid != (uint32_t)-1 && s.best_ask != (uint32_t)-1 && s.best_ask >= s.best_bid)
                               ? s.best_ask - s.best_bid
                               : 0;
                snaps.push_back(s);
            }
        }

        // TUI refresh every ~80ms
        if (cfg.tui)
        {
            auto now = Clock::now();
            if (now - last_tui_update > std::chrono::milliseconds(80) || i + 1 == rows.size())
            {
                last_tui_update = now;
                double pct = 100.0 * (i + 1) / rows.size();
                double elapsed_ms = wall_ns / 1e6;
                double rate = (i + 1) / (elapsed_ms / 1000.0 + 0.001);
                uint64_t bb = core.bestBid(cfg.watch_instrument ? cfg.watch_instrument : instruments[0].instrumentId);
                uint64_t ba = core.bestAsk(cfg.watch_instrument ? cfg.watch_instrument : instruments[0].instrumentId);

                std::cout << "\033[H"; // home
                std::cout << tui::BOLD << tui::CYAN
                          << "┌─ ORDER MATCHING ENGINE · LIVE SIMULATION ─────────────────────────────────┐\n"
                          << tui::RESET;
                std::cout << "│ " << tui::GRAY << "cmds " << tui::RESET
                          << std::setw(7) << (i + 1) << "/" << std::setw(7) << std::left << rows.size() << std::right
                          << "  " << tui::bar(pct / 100.0, 30, tui::GREEN)
                          << " " << std::fixed << std::setprecision(1) << std::setw(5) << pct << "%   │\n";
                std::cout << "│ " << tui::GRAY << "rate " << tui::RESET << tui::BOLD << tui::GREEN
                          << std::setw(10) << (uint64_t)rate << tui::RESET << " cmd/s"
                          << "   " << tui::GRAY << "elapsed " << tui::RESET
                          << std::fixed << std::setprecision(1) << std::setw(8) << elapsed_ms << " ms"
                          << "               │\n";
                std::cout << "├─ EVENTS ──────────────────────────────────────────────────────────────────┤\n";
                std::cout << "│ " << tui::GREEN << "fill      " << std::setw(8) << ev_fill.load() << tui::RESET
                          << "  " << tui::GREEN << "partial   " << std::setw(8) << ev_partial.load() << tui::RESET
                          << "  " << tui::BLUE << "accept   " << std::setw(8) << ev_accept.load() << tui::RESET
                          << "  │\n";
                std::cout << "│ " << tui::YELLOW << "cancel    " << std::setw(8) << ev_cancel.load() << tui::RESET
                          << "  " << tui::RED << "reject    " << std::setw(8) << ev_reject.load() << tui::RESET
                          << "  " << tui::CYAN << "volume   " << std::setw(8) << ev_volume.load() << tui::RESET
                          << "  │\n";
                std::cout << "├─ TOP OF BOOK (instrument " << (cfg.watch_instrument ? cfg.watch_instrument : instruments[0].instrumentId)
                          << ") ──────────────────────────────────────────────┤\n";
                if (bb != (uint32_t)-1 && ba != (uint32_t)-1)
                {
                    std::cout << "│ " << tui::GRAY << "bid " << tui::GREEN << tui::BOLD
                              << std::setw(10) << bb << tui::RESET
                              << "   " << tui::GRAY << "ask " << tui::RED << tui::BOLD
                              << std::setw(10) << ba << tui::RESET
                              << "   " << tui::GRAY << "spread " << tui::RESET << tui::YELLOW
                              << std::setw(6) << (ba - bb) << tui::RESET
                              << "           │\n";
                }
                else
                {
                    std::cout << "│ " << tui::GRAY << "book empty                                                         " << tui::RESET << "│\n";
                }
                std::cout << "└───────────────────────────────────────────────────────────────────────────┘\n";
                std::cout << tui::DIM << "   (live view; full stats + JSON written on completion)" << tui::RESET << "    \n";
                std::cout.flush();
            }
        }
        else if ((i + 1) % 5000 == 0 || i + 1 == rows.size())
        {
            double pct = 100.0 * (i + 1) / rows.size();
            double el = wall_ns / 1e6;
            double rate = (i + 1) / (el / 1000.0 + 0.001);
            std::cout << "\r[SIM] " << (i + 1) << "/" << rows.size()
                      << " (" << std::fixed << std::setprecision(1) << pct << "%) "
                      << std::setprecision(0) << rate << " cmd/s     " << std::flush;
        }
    }

    std::cout << "\n[SIM] all commands submitted, draining events...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let engine finish

    auto sim_t1 = Clock::now();
    uint64_t total_ns = std::chrono::duration_cast<ns>(sim_t1 - sim_t0).count();

    // Final snapshot
    for (const auto &inst : instruments)
    {
        if (cfg.watch_instrument != 0 && inst.instrumentId != cfg.watch_instrument)
            continue;
        Snap s{};
        s.wall_ns = total_ns;
        s.instrument_id = inst.instrumentId;
        s.best_bid = core.bestBid(inst.instrumentId);
        s.best_ask = core.bestAsk(inst.instrumentId);
        s.spread = (s.best_bid != (uint32_t)-1 && s.best_ask != (uint32_t)-1 && s.best_ask >= s.best_bid)
                       ? s.best_ask - s.best_bid
                       : 0;
        snaps.push_back(s);
    }

    core.stop();
    if (cfg.tui)
        std::cout << tui::SHOW_CURSOR;

    // ── Correlate events → command results ──────────────────────────────
    {
        std::lock_guard<std::mutex> lk(ev_mu);
        for (auto &[wall_ns, ev] : raw_events)
        {
            auto it = oid_to_idx.find(ev.aggressorOrderId);
            if (it == oid_to_idx.end())
                continue;
            auto &r = results[it->second];
            if (r.first_event_ns == 0 || wall_ns < r.first_event_ns)
                r.first_event_ns = wall_ns;
            if (wall_ns > r.last_event_ns)
                r.last_event_ns = wall_ns;
            r.events.push_back(ev);
        }
    }

    std::vector<uint64_t> e2e_lat;
    e2e_lat.reserve(results.size());
    for (auto &r : results)
    {
        if (r.last_event_ns > 0 && r.last_event_ns >= r.submit_wall_ns)
        {
            r.e2e_latency_ns = r.last_event_ns - r.submit_wall_ns;
            e2e_lat.push_back(r.e2e_latency_ns);
        }
    }

    auto sub_stats = compute(submit_lat);
    auto e2e_stats = compute(e2e_lat);

    double total_ms = total_ns / 1e6;
    double throughput = rows.size() / (total_ms / 1000.0);

    // ── Print summary ───────────────────────────────────────────────────
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════\n";
    std::cout << "  SIMULATION RESULTS\n";
    std::cout << "═══════════════════════════════════════════════════════════\n";
    std::cout << "  commands:         " << rows.size() << "\n";
    std::cout << "  events total:     " << ev_total.load() << "\n";
    std::cout << "    fills:          " << ev_fill.load() << "\n";
    std::cout << "    partial fills:  " << ev_partial.load() << "\n";
    std::cout << "    accepted:       " << ev_accept.load() << "\n";
    std::cout << "    rejected:       " << ev_reject.load() << "\n";
    std::cout << "    cancelled:      " << ev_cancel.load() << "\n";
    std::cout << "  volume:           " << ev_volume.load() << "\n";
    std::cout << "  turnover:         " << ev_turnover.load() << "\n";
    std::cout << "  sim time:         " << std::fixed << std::setprecision(2) << total_ms << " ms\n";
    std::cout << "  throughput:       " << std::setprecision(0) << throughput << " cmd/s\n";
    std::cout << "\n  SUBMIT LATENCY (gateway enqueue):\n";
    std::cout << "    mean=" << (uint64_t)sub_stats.mean << "ns  p50=" << (uint64_t)sub_stats.p50
              << "  p95=" << (uint64_t)sub_stats.p95 << "  p99=" << (uint64_t)sub_stats.p99
              << "  p99.9=" << (uint64_t)sub_stats.p999 << "  max=" << sub_stats.max_v << "\n";
    std::cout << "\n  E2E LATENCY (submit → last event):\n";
    std::cout << "    mean=" << (uint64_t)e2e_stats.mean << "ns  p50=" << (uint64_t)e2e_stats.p50
              << "  p95=" << (uint64_t)e2e_stats.p95 << "  p99=" << (uint64_t)e2e_stats.p99
              << "  p99.9=" << (uint64_t)e2e_stats.p999 << "  max=" << e2e_stats.max_v << "\n";
    std::cout << "═══════════════════════════════════════════════════════════\n";

    // ── Write JSON output ───────────────────────────────────────────────
    std::ofstream out(cfg.output_file);
    if (!out)
    {
        std::cerr << "cannot open " << cfg.output_file << "\n";
        return 1;
    }

    auto writeStats = [&](const char *name, const Stats &s)
    {
        out << "    \"" << name << "\": {"
            << "\"count\":" << s.count
            << ",\"mean_ns\":" << (uint64_t)s.mean
            << ",\"stddev_ns\":" << (uint64_t)s.stddev
            << ",\"p50_ns\":" << (uint64_t)s.p50
            << ",\"p90_ns\":" << (uint64_t)s.p90
            << ",\"p95_ns\":" << (uint64_t)s.p95
            << ",\"p99_ns\":" << (uint64_t)s.p99
            << ",\"p999_ns\":" << (uint64_t)s.p999
            << ",\"min_ns\":" << s.min_v
            << ",\"max_ns\":" << s.max_v << "}";
    };

    out << "{\n";
    out << "  \"config\": {\"mode\":\"" << (cfg.realtime ? "realtime" : "burst")
        << "\",\"workers\":" << cfg.num_workers
        << ",\"snapshot_ms\":" << cfg.snapshot_ms
        << ",\"depth_levels\":" << cfg.depth_levels
        << ",\"watch_instrument\":" << cfg.watch_instrument
        << "},\n";

    out << "  \"instruments\": [";
    for (size_t i = 0; i < instruments.size(); ++i)
    {
        const auto &inst = instruments[i];
        out << "{\"id\":" << inst.instrumentId
            << ",\"min_price\":" << inst.priceRange.minPrice
            << ",\"max_price\":" << inst.priceRange.maxPrice
            << ",\"tick_size\":" << inst.priceRange.tickSize << "}";
        if (i + 1 < instruments.size())
            out << ",";
    }
    out << "],\n";

    out << "  \"summary\": {\n";
    out << "    \"total_commands\":" << rows.size()
        << ",\"total_events\":" << ev_total.load()
        << ",\"fills\":" << ev_fill.load()
        << ",\"partial_fills\":" << ev_partial.load()
        << ",\"accepted\":" << ev_accept.load()
        << ",\"rejected\":" << ev_reject.load()
        << ",\"cancelled\":" << ev_cancel.load()
        << ",\"volume\":" << ev_volume.load()
        << ",\"turnover\":" << ev_turnover.load()
        << ",\"simulation_time_ns\":" << total_ns
        << ",\"simulation_time_ms\":" << total_ms
        << ",\"throughput_cmds_per_sec\":" << (uint64_t)throughput << ",\n";
    writeStats("submit_latency", sub_stats);
    out << ",\n";
    writeStats("e2e_latency", e2e_stats);
    out << "\n  },\n";

    // Throughput timeline (1ms buckets)
    uint64_t bucket_ns = 1'000'000;
    size_t nbuckets = (total_ns / bucket_ns) + 1;
    if (nbuckets > 100000)
        nbuckets = 100000;
    std::vector<uint32_t> tp_buckets(nbuckets, 0);
    for (const auto &r : results)
    {
        size_t b = r.submit_wall_ns / bucket_ns;
        if (b < tp_buckets.size())
            tp_buckets[b]++;
    }
    out << "  \"throughput_timeline\": [";
    bool first = true;
    for (size_t i = 0; i < tp_buckets.size(); ++i)
    {
        if (!first)
            out << ",";
        out << "{\"t_ms\":" << i << ",\"cmds\":" << tp_buckets[i] << "}";
        first = false;
    }
    out << "],\n";

    // Book snapshots
    out << "  \"book_snapshots\": [";
    for (size_t i = 0; i < snaps.size(); ++i)
    {
        const auto &s = snaps[i];
        out << "{\"wall_ns\":" << s.wall_ns
            << ",\"instrument_id\":" << s.instrument_id
            << ",\"best_bid\":" << (int64_t)(s.best_bid == (uint32_t)-1 ? -1 : (int64_t)s.best_bid)
            << ",\"best_ask\":" << (int64_t)(s.best_ask == (uint32_t)-1 ? -1 : (int64_t)s.best_ask)
            << ",\"spread\":" << s.spread << "}";
        if (i + 1 < snaps.size())
            out << ",";
    }
    out << "],\n";

    // Per-command records
    out << "  \"commands\": [\n";
    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto &r = results[i];
        out << "    {\"seq\":" << r.seq
            << ",\"scheduled_ns\":" << r.scheduled_ns
            << ",\"submit_wall_ns\":" << r.submit_wall_ns
            << ",\"submit_latency_ns\":" << r.submit_latency_ns
            << ",\"e2e_latency_ns\":" << r.e2e_latency_ns
            << ",\"type\":\"" << cmdName(r.cmd.type) << "\""
            << ",\"order_id\":" << r.cmd.orderId
            << ",\"instrument_id\":" << r.cmd.instrumentId
            << ",\"client_id\":" << r.cmd.clientId
            << ",\"verb\":\"" << verbName(r.cmd.verb) << "\""
            << ",\"order_type\":\"" << otName(r.cmd.orderType) << "\""
            << ",\"tif\":\"" << tifName(r.cmd.tif) << "\""
            << ",\"limit_price\":" << r.cmd.limitPrice
            << ",\"qty\":" << r.cmd.qty
            << ",\"events\":[";
        for (size_t j = 0; j < r.events.size(); ++j)
        {
            const auto &ev = r.events[j];
            out << "{\"type\":\"" << evName(ev.type) << "\"";
            if (ev.fillPrice)
                out << ",\"fill_price\":" << ev.fillPrice;
            if (ev.fillQty)
                out << ",\"fill_qty\":" << ev.fillQty;
            if (ev.passiveOrderId)
                out << ",\"passive_oid\":" << ev.passiveOrderId;
            if (ev.aggressorRemaining)
                out << ",\"agg_rem\":" << ev.aggressorRemaining;
            if (ev.passiveRemaining)
                out << ",\"pass_rem\":" << ev.passiveRemaining;
            out << "}";
            if (j + 1 < r.events.size())
                out << ",";
        }
        out << "]}";
        if (i + 1 < results.size())
            out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";

    std::cout << "[SIM] wrote " << cfg.output_file << "\n";
    return 0;
}