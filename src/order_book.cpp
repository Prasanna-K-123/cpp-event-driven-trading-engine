#include "qdev/order_book.hpp"

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace qdev {

namespace {

std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
    return h;
}

void prepare_fill_buffer(std::vector<Fill>& fills) {
    fills.clear();
    if (fills.capacity() < 4) fills.reserve(4);
}

}  // namespace

void OrderBook::reserve_order_capacity(std::size_t expected_live_orders) {
    orders_.max_load_factor(0.70F);
    orders_.reserve(expected_live_orders);
}

void OrderBook::rest(OrderId id, Side side, PriceTicks price_ticks, Quantity quantity,
                     std::uint64_t arrival_sequence) {
    if (orders_.contains(id)) {
        throw std::invalid_argument("duplicate live order id");
    }

    LevelState* level = nullptr;
    if (side == Side::Buy) level = &bids_[price_ticks];
    else level = &asks_[price_ticks];

    OrderState state{side, price_ticks, quantity, arrival_sequence, level->tail_id, 0};
    auto [order_it, inserted] = orders_.emplace(id, state);
    if (!inserted) throw std::logic_error("order insertion failed unexpectedly");

    if (level->tail_id != 0) {
        auto tail_it = orders_.find(level->tail_id);
        if (tail_it == orders_.end()) throw std::logic_error("level tail missing from order index");
        tail_it->second.next_id = id;
    } else {
        level->head_id = id;
    }
    level->tail_id = id;
    level->total_quantity += quantity;
    ++level->order_count;
}

void OrderBook::unlink_order(OrderMap::iterator order_it) {
    const OrderState state = order_it->second;
    LevelState* level = nullptr;
    if (state.side == Side::Buy) {
        auto level_it = bids_.find(state.price_ticks);
        if (level_it == bids_.end()) throw std::logic_error("bid level missing during unlink");
        level = &level_it->second;
    } else {
        auto level_it = asks_.find(state.price_ticks);
        if (level_it == asks_.end()) throw std::logic_error("ask level missing during unlink");
        level = &level_it->second;
    }

    if (state.prev_id != 0) {
        auto prev_it = orders_.find(state.prev_id);
        if (prev_it == orders_.end()) throw std::logic_error("previous FIFO order missing");
        prev_it->second.next_id = state.next_id;
    } else {
        level->head_id = state.next_id;
    }

    if (state.next_id != 0) {
        auto next_it = orders_.find(state.next_id);
        if (next_it == orders_.end()) throw std::logic_error("next FIFO order missing");
        next_it->second.prev_id = state.prev_id;
    } else {
        level->tail_id = state.prev_id;
    }

    if (level->order_count == 0) throw std::logic_error("level order count underflow");
    --level->order_count;
    orders_.erase(order_it);

    if (level->order_count == 0) {
        if (level->head_id != 0 || level->tail_id != 0 || level->total_quantity != 0) {
            throw std::logic_error("empty level invariant violated");
        }
        if (state.side == Side::Buy) bids_.erase(state.price_ticks);
        else asks_.erase(state.price_ticks);
    }
}

Quantity OrderBook::consume_asks(Quantity quantity, std::optional<PriceTicks> max_price,
                                 std::vector<Fill>& fills) {
    Quantity remaining = quantity;
    while (remaining > 0 && !asks_.empty()) {
        auto level_it = asks_.begin();
        if (max_price && level_it->first > *max_price) break;
        auto& level = level_it->second;
        if (level.head_id == 0) throw std::logic_error("nonempty ask level has no head order");
        auto order_it = orders_.find(level.head_id);
        if (order_it == orders_.end()) throw std::logic_error("ask head missing from order index");
        auto& maker = order_it->second;
        if (maker.side != Side::Sell || maker.price_ticks != level_it->first || maker.remaining <= 0) {
            throw std::logic_error("ask FIFO/index invariant violated");
        }

        const Quantity fill_qty = std::min(remaining, maker.remaining);
        maker.remaining -= fill_qty;
        level.total_quantity -= fill_qty;
        remaining -= fill_qty;
        fills.push_back(Fill{order_it->first, maker.side, maker.price_ticks, fill_qty});
        if (maker.remaining == 0) unlink_order(order_it);
    }
    return remaining;
}

Quantity OrderBook::consume_bids(Quantity quantity, std::optional<PriceTicks> min_price,
                                 std::vector<Fill>& fills) {
    Quantity remaining = quantity;
    while (remaining > 0 && !bids_.empty()) {
        auto level_it = bids_.begin();
        if (min_price && level_it->first < *min_price) break;
        auto& level = level_it->second;
        if (level.head_id == 0) throw std::logic_error("nonempty bid level has no head order");
        auto order_it = orders_.find(level.head_id);
        if (order_it == orders_.end()) throw std::logic_error("bid head missing from order index");
        auto& maker = order_it->second;
        if (maker.side != Side::Buy || maker.price_ticks != level_it->first || maker.remaining <= 0) {
            throw std::logic_error("bid FIFO/index invariant violated");
        }

        const Quantity fill_qty = std::min(remaining, maker.remaining);
        maker.remaining -= fill_qty;
        level.total_quantity -= fill_qty;
        remaining -= fill_qty;
        fills.push_back(Fill{order_it->first, maker.side, maker.price_ticks, fill_qty});
        if (maker.remaining == 0) unlink_order(order_it);
    }
    return remaining;
}

void OrderBook::add_limit(OrderId id, Side side, PriceTicks price_ticks, Quantity quantity,
                          std::uint64_t arrival_sequence, std::vector<Fill>& fills) {
    if (id == 0 || price_ticks <= 0 || quantity <= 0) throw std::invalid_argument("invalid limit order");
    if (orders_.contains(id)) throw std::invalid_argument("duplicate live order id");

    prepare_fill_buffer(fills);
    Quantity remaining = quantity;
    if (side == Side::Buy) remaining = consume_asks(quantity, price_ticks, fills);
    else remaining = consume_bids(quantity, price_ticks, fills);
    if (remaining > 0) rest(id, side, price_ticks, remaining, arrival_sequence);
}

std::vector<Fill> OrderBook::add_limit(OrderId id, Side side, PriceTicks price_ticks, Quantity quantity,
                                       std::uint64_t arrival_sequence) {
    std::vector<Fill> fills;
    add_limit(id, side, price_ticks, quantity, arrival_sequence, fills);
    return fills;
}

Quantity OrderBook::cancel(OrderId id, Quantity quantity) {
    auto order_it = orders_.find(id);
    if (order_it == orders_.end() || quantity <= 0) return 0;
    auto& order = order_it->second;
    const Quantity cancelled = std::min(quantity, order.remaining);
    order.remaining -= cancelled;

    if (order.side == Side::Buy) {
        auto level_it = bids_.find(order.price_ticks);
        if (level_it == bids_.end()) throw std::logic_error("bid level missing during cancel");
        level_it->second.total_quantity -= cancelled;
    } else {
        auto level_it = asks_.find(order.price_ticks);
        if (level_it == asks_.end()) throw std::logic_error("ask level missing during cancel");
        level_it->second.total_quantity -= cancelled;
    }

    if (order.remaining == 0) unlink_order(order_it);
    return cancelled;
}

void OrderBook::execute_market(Side aggressor_side, Quantity quantity, std::vector<Fill>& fills) {
    if (quantity <= 0) throw std::invalid_argument("market quantity must be positive");
    prepare_fill_buffer(fills);
    if (aggressor_side == Side::Buy) static_cast<void>(consume_asks(quantity, std::nullopt, fills));
    else static_cast<void>(consume_bids(quantity, std::nullopt, fills));
}

std::vector<Fill> OrderBook::execute_market(Side aggressor_side, Quantity quantity) {
    std::vector<Fill> fills;
    execute_market(aggressor_side, quantity, fills);
    return fills;
}

std::optional<PriceTicks> OrderBook::best_bid() const noexcept {
    return bids_.empty() ? std::nullopt : std::optional<PriceTicks>{bids_.begin()->first};
}

std::optional<PriceTicks> OrderBook::best_ask() const noexcept {
    return asks_.empty() ? std::nullopt : std::optional<PriceTicks>{asks_.begin()->first};
}

Quantity OrderBook::best_bid_quantity() const noexcept { return first_level_quantity(bids_); }
Quantity OrderBook::best_ask_quantity() const noexcept { return first_level_quantity(asks_); }

Quantity OrderBook::total_resting_quantity() const noexcept {
    Quantity total = 0;
    for (const auto& [_, level] : bids_) total += level.total_quantity;
    for (const auto& [_, level] : asks_) total += level.total_quantity;
    return total;
}

bool OrderBook::is_crossed() const noexcept {
    const auto bid = best_bid();
    const auto ask = best_ask();
    return bid && ask && *bid >= *ask;
}

const OrderState* OrderBook::find_order(OrderId id) const noexcept {
    const auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
}

std::uint64_t OrderBook::state_checksum() const {
    std::uint64_t h = 0x84222325cbf29ce4ULL;
    using Snapshot = std::tuple<OrderId, std::uint8_t, PriceTicks, Quantity, std::uint64_t, OrderId, OrderId>;
    std::vector<Snapshot> live;
    live.reserve(orders_.size());
    for (const auto& [id, state] : orders_) {
        live.emplace_back(id, static_cast<std::uint8_t>(state.side), state.price_ticks,
                          state.remaining, state.arrival_sequence, state.prev_id, state.next_id);
    }
    std::sort(live.begin(), live.end());
    for (const auto& [id, side, price, qty, seq, prev, next] : live) {
        h = mix(h, id);
        h = mix(h, side);
        h = mix(h, static_cast<std::uint64_t>(price));
        h = mix(h, static_cast<std::uint64_t>(qty));
        h = mix(h, seq);
        h = mix(h, prev);
        h = mix(h, next);
    }
    return h;
}

}  // namespace qdev
