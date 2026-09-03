#pragma once

#include "qdev/order_book.hpp"
#include "qdev/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qdev {

struct EngineStats {
    std::uint64_t events_processed{};
    std::uint64_t adds{};
    std::uint64_t cancels{};
    std::uint64_t market_orders{};
    std::uint64_t fills{};
    std::uint64_t filled_quantity{};
    std::uint64_t rejected_cancels{};
};

class EventEngine {
public:
    void process(const Event& event);
    void replay(const std::vector<Event>& events);
    void reserve_order_capacity(std::size_t expected_live_orders) { book_.reserve_order_capacity(expected_live_orders); }

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t state_checksum() const noexcept;

private:
    OrderBook book_;
    EngineStats stats_;
    std::vector<Fill> fill_scratch_;
    TimestampNs last_ts_ns_{};
    std::uint64_t last_sequence_{};
    bool started_{false};

    void account_fills(const std::vector<Fill>& fills);
};

}  // namespace qdev
