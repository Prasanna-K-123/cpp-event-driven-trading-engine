#pragma once

#include "qdev/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace qdev {

struct OrderState {
    Side side{Side::Buy};
    PriceTicks price_ticks{};
    Quantity remaining{};
    std::uint64_t arrival_sequence{};
    OrderId prev_id{};
    OrderId next_id{};
};

struct LevelState {
    Quantity total_quantity{};
    OrderId head_id{};
    OrderId tail_id{};
    std::size_t order_count{};
};

class OrderBook {
public:
    void add_limit(OrderId id, Side side, PriceTicks price_ticks, Quantity quantity,
                   std::uint64_t arrival_sequence, std::vector<Fill>& fills);
    std::vector<Fill> add_limit(OrderId id, Side side, PriceTicks price_ticks, Quantity quantity,
                                std::uint64_t arrival_sequence);
    Quantity cancel(OrderId id, Quantity quantity);
    void execute_market(Side aggressor_side, Quantity quantity, std::vector<Fill>& fills);
    std::vector<Fill> execute_market(Side aggressor_side, Quantity quantity);

    void reserve_order_capacity(std::size_t expected_live_orders);

    [[nodiscard]] std::optional<PriceTicks> best_bid() const noexcept;
    [[nodiscard]] std::optional<PriceTicks> best_ask() const noexcept;
    [[nodiscard]] Quantity best_bid_quantity() const noexcept;
    [[nodiscard]] Quantity best_ask_quantity() const noexcept;
    [[nodiscard]] std::size_t live_order_count() const noexcept { return orders_.size(); }
    [[nodiscard]] Quantity total_resting_quantity() const noexcept;
    [[nodiscard]] bool is_crossed() const noexcept;
    [[nodiscard]] const OrderState* find_order(OrderId id) const noexcept;
    [[nodiscard]] std::uint64_t state_checksum() const;

private:
    using BidMap = std::map<PriceTicks, LevelState, std::greater<PriceTicks>>;
    using AskMap = std::map<PriceTicks, LevelState, std::less<PriceTicks>>;
    using OrderMap = std::unordered_map<OrderId, OrderState>;

    BidMap bids_;
    AskMap asks_;
    OrderMap orders_;

    void rest(OrderId id, Side side, PriceTicks price_ticks, Quantity quantity,
              std::uint64_t arrival_sequence);
    void unlink_order(OrderMap::iterator order_it);
    Quantity consume_asks(Quantity quantity, std::optional<PriceTicks> max_price, std::vector<Fill>& fills);
    Quantity consume_bids(Quantity quantity, std::optional<PriceTicks> min_price, std::vector<Fill>& fills);

    template <typename BookSide>
    static Quantity first_level_quantity(const BookSide& side) noexcept {
        return side.empty() ? 0 : side.begin()->second.total_quantity;
    }
};

}  // namespace qdev
