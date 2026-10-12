#include "qdev/engine.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
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

template <typename Integer>
Integer parse_integer(const std::string& value, const char* field) {
    if (value.empty()) throw std::invalid_argument(std::string(field) + " is empty");
    Integer result{};
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::invalid_argument(std::string(field) + " requires a complete, in-range decimal integer");
    }
    return result;
}

qdev::Event parse_line(const std::string& line) {
    std::array<std::string, 7> columns{};
    std::size_t from = 0;
    for (std::size_t i = 0; i < columns.size() - 1; ++i) {
        const auto comma = line.find(',', from);
        if (comma == std::string::npos) {
            throw std::invalid_argument("replay CSV requires exactly seven fields");
        }
        columns[i] = line.substr(from, comma - from);
        from = comma + 1;
    }
    if (line.find(',', from) != std::string::npos) {
        throw std::invalid_argument("replay CSV requires exactly seven fields");
    }
    columns.back() = line.substr(from);
    qdev::Event event;
    event.ts_ns = parse_integer<std::uint64_t>(columns[0], "ts_ns");
    event.sequence = parse_integer<std::uint64_t>(columns[1], "sequence");
    event.type = parse_type(columns[2]);
    event.side = parse_side(columns[3]);
    event.order_id = parse_integer<std::uint64_t>(columns[4], "order_id");
    event.price_ticks = parse_integer<std::int64_t>(columns[5], "price_ticks");
    event.quantity = parse_integer<std::int64_t>(columns[6], "quantity");
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
    std::size_t line_number = 0;
    constexpr const char* header = "ts_ns,sequence,type,side,order_id,price_ticks,quantity";
    while (std::getline(in, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\\r') line.pop_back();
        if (line.empty()) continue;  // preserve historical empty-line skip policy
        if (first && line == header) {
            first = false;
            continue;
        }
        first = false;
        try {
            engine.process(parse_line(line));
        } catch (const std::exception& error) {
            std::cerr << "replay CSV row " << line_number << ": " << error.what() << '\\n';
            return 2;
        }
    }
    if (in.bad()) {
        std::cerr << "replay CSV read failure after row " << line_number << '\\n';
        return 2;
    }

    const auto& stats = engine.stats();
    std::cout << "events_processed=" << stats.events_processed << '\n'
              << "fills=" << stats.fills << '\n'
              << "filled_quantity=" << stats.filled_quantity << '\n'
              << "live_orders=" << engine.book().live_order_count() << '\n'
              << "checksum=" << engine.state_checksum() << '\n';
    return 0;
}
