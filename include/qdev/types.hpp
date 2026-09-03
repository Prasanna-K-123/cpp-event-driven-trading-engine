#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace qdev {

using TimestampNs = std::uint64_t;
using OrderId = std::uint64_t;
using PriceTicks = std::int64_t;
using Quantity = std::int64_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class EventType : std::uint8_t { Add = 0, Cancel = 1, Market = 2 };

constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

inline std::string_view to_string(Side side) noexcept {
    return side == Side::Buy ? "BUY" : "SELL";
}

inline std::string_view to_string(EventType type) noexcept {
    switch (type) {
        case EventType::Add: return "ADD";
        case EventType::Cancel: return "CANCEL";
        case EventType::Market: return "MARKET";
    }
    return "UNKNOWN";
}

struct Event {
    TimestampNs ts_ns{};
    std::uint64_t sequence{};
    EventType type{EventType::Add};
    Side side{Side::Buy};
    OrderId order_id{};
    PriceTicks price_ticks{};
    Quantity quantity{};
};

struct Fill {
    OrderId maker_order_id{};
    Side maker_side{Side::Buy};
    PriceTicks price_ticks{};
    Quantity quantity{};
};

inline void validate_event(const Event& event) {
    if (event.quantity <= 0) {
        throw std::invalid_argument("event quantity must be positive");
    }
    if (event.type == EventType::Add && (event.order_id == 0 || event.price_ticks <= 0)) {
        throw std::invalid_argument("add event requires nonzero order id and positive price");
    }
    if (event.type == EventType::Cancel && event.order_id == 0) {
        throw std::invalid_argument("cancel event requires nonzero order id");
    }
}

}  // namespace qdev
