#include "qdev/engine.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

qdev::Side parse_side(const std::string& value) {
    if (value == "BUY") return qdev::Side::Buy;
    if (value == "SELL") return qdev::Side::Sell;
    throw std::invalid_argument("side must be BUY or SELL");
}

qdev::EventType parse_type(const std::string& value) {
    if (value == "ADD") return qdev::EventType::Add;
    if (value == "CANCEL") return qdev::EventType::Cancel;
    if (value == "MARKET") return qdev::EventType::Market;
    throw std::invalid_argument("type must be ADD, CANCEL, or MARKET");
}

qdev::Event parse_line(const std::string& line) {
    std::stringstream ss(line);
    std::string token;
    qdev::Event event;
    std::getline(ss, token, ','); event.ts_ns = std::stoull(token);
    std::getline(ss, token, ','); event.sequence = std::stoull(token);
    std::getline(ss, token, ','); event.type = parse_type(token);
    std::getline(ss, token, ','); event.side = parse_side(token);
    std::getline(ss, token, ','); event.order_id = std::stoull(token);
    std::getline(ss, token, ','); event.price_ticks = std::stoll(token);
    std::getline(ss, token, ','); event.quantity = std::stoll(token);
    return event;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: qdev_replay events.csv\n";
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "could not open input\n";
        return 2;
    }

    qdev::EventEngine engine;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (first && line.rfind("ts_ns,", 0) == 0) {
            first = false;
            continue;
        }
        first = false;
        engine.process(parse_line(line));
    }

    const auto& stats = engine.stats();
    std::cout << "events_processed=" << stats.events_processed << '\n'
              << "fills=" << stats.fills << '\n'
              << "filled_quantity=" << stats.filled_quantity << '\n'
              << "live_orders=" << engine.book().live_order_count() << '\n'
              << "checksum=" << engine.state_checksum() << '\n';
    return 0;
}
