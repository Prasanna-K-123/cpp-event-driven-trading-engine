#include "qdev/engine.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef QDEV_CALLGRIND
#include <valgrind/callgrind.h>
#endif

namespace {

struct Config {
    std::size_t events{1'000'000};
    std::uint64_t seed{20260902};
    std::string json_path;
};

Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--events" && i + 1 < argc) cfg.events = std::stoull(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc) cfg.seed = std::stoull(argv[++i]);
        else if (arg == "--json" && i + 1 < argc) cfg.json_path = argv[++i];
        else {
            std::cerr << "unknown/incomplete argument: " << arg << '\n';
            std::exit(2);
        }
    }
    return cfg;
}

std::vector<qdev::Event> generate_events(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> qty_dist(1, 20);
    std::uniform_int_distribution<int> price_offset(-20, 20);
    std::uniform_int_distribution<int> event_dist(0, 99);

    std::vector<qdev::Event> out;
    out.reserve(count + 512);
    std::vector<qdev::OrderId> live_ids;
    live_ids.reserve(count / 2 + 1024);
    qdev::OrderId next_id = 1;
    std::uint64_t seq = 1;
    qdev::TimestampNs ts = 1'000'000'000ULL;
    constexpr qdev::PriceTicks mid = 100'000;

    for (int level = 1; level <= 64 && out.size() < count; ++level) {
        for (qdev::Side side : {qdev::Side::Buy, qdev::Side::Sell}) {
            const qdev::PriceTicks px = mid + (side == qdev::Side::Buy ? -level : level);
            out.push_back(qdev::Event{ts++, seq++, qdev::EventType::Add, side, next_id, px, 50});
            live_ids.push_back(next_id++);
            if (out.size() == count) return out;
        }
    }

    while (out.size() < count) {
        const int pick = event_dist(rng);
        const qdev::Side side = side_dist(rng) == 0 ? qdev::Side::Buy : qdev::Side::Sell;
        const qdev::Quantity qty = qty_dist(rng);
        if (pick < 70 || live_ids.empty()) {
            const int offset = std::abs(price_offset(rng)) + 1;
            const qdev::PriceTicks px = mid + (side == qdev::Side::Buy ? -offset : offset);
            out.push_back(qdev::Event{ts, seq, qdev::EventType::Add, side, next_id, px, qty});
            live_ids.push_back(next_id++);
        } else if (pick < 85) {
            const std::size_t idx = static_cast<std::size_t>(rng() % live_ids.size());
            const qdev::OrderId id = live_ids[idx];
            out.push_back(qdev::Event{ts, seq, qdev::EventType::Cancel, side, id, 1, qty});
            if ((rng() & 3ULL) == 0ULL) {
                live_ids[idx] = live_ids.back();
                live_ids.pop_back();
            }
        } else {
            out.push_back(qdev::Event{ts, seq, qdev::EventType::Market, side, 0, 1, qty});
        }
        ts += 100;
        ++seq;
    }
    return out;
}

void write_json(const std::string& path, const Config& cfg, double elapsed_ms,
                double throughput, double ns_per_event, const qdev::EventEngine& engine) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("could not open benchmark JSON output");
    out << std::fixed << std::setprecision(3)
        << "{\n"
        << "  \"events\": " << cfg.events << ",\n"
        << "  \"seed\": " << cfg.seed << ",\n"
        << "  \"elapsed_ms\": " << elapsed_ms << ",\n"
        << "  \"throughput_events_per_sec\": " << throughput << ",\n"
        << "  \"average_ns_per_event\": " << ns_per_event << ",\n"
        << "  \"fills\": " << engine.stats().fills << ",\n"
        << "  \"filled_quantity\": " << engine.stats().filled_quantity << ",\n"
        << "  \"rejected_cancels\": " << engine.stats().rejected_cancels << ",\n"
        << "  \"live_orders\": " << engine.book().live_order_count() << ",\n"
        << "  \"resting_quantity\": " << engine.book().total_resting_quantity() << ",\n"
        << "  \"state_checksum\": " << engine.state_checksum() << "\n"
        << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    const Config cfg = parse_args(argc, argv);
    const auto events = generate_events(cfg.events, cfg.seed);
    qdev::EventEngine engine;
    engine.reserve_order_capacity(cfg.events * 3 / 4 + 256);

#ifdef QDEV_CALLGRIND
    CALLGRIND_START_INSTRUMENTATION;
    CALLGRIND_ZERO_STATS;
#endif
    const auto start = std::chrono::steady_clock::now();
    engine.replay(events);
    const auto stop = std::chrono::steady_clock::now();
#ifdef QDEV_CALLGRIND
    CALLGRIND_STOP_INSTRUMENTATION;
#endif

    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
    const double elapsed_ms = static_cast<double>(elapsed_ns) / 1e6;
    const double throughput = static_cast<double>(cfg.events) * 1e9 / static_cast<double>(elapsed_ns);
    const double ns_per_event = static_cast<double>(elapsed_ns) / static_cast<double>(cfg.events);

    std::cout << std::fixed << std::setprecision(3)
              << "events=" << cfg.events << '\n'
              << "elapsed_ms=" << elapsed_ms << '\n'
              << "throughput_events_per_sec=" << throughput << '\n'
              << "average_ns_per_event=" << ns_per_event << '\n'
              << "fills=" << engine.stats().fills << '\n'
              << "rejected_cancels=" << engine.stats().rejected_cancels << '\n'
              << "live_orders=" << engine.book().live_order_count() << '\n'
              << "checksum=" << engine.state_checksum() << '\n';

    if (!cfg.json_path.empty()) write_json(cfg.json_path, cfg, elapsed_ms, throughput, ns_per_event, engine);
    return 0;
}
