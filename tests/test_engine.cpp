#include "qdev/engine.hpp"
#include "qdev/order_book.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void test_price_time_priority() {
    qdev::OrderBook book;
    book.add_limit(1, qdev::Side::Sell, 101, 5, 1);
    book.add_limit(2, qdev::Side::Sell, 101, 7, 2);
    const auto fills = book.execute_market(qdev::Side::Buy, 9);
    expect(fills.size() == 2, "market buy should produce two fills");
    expect(fills[0].maker_order_id == 1 && fills[0].quantity == 5, "first maker should fill first");
    expect(fills[1].maker_order_id == 2 && fills[1].quantity == 4, "second maker should receive residual");
    const auto* second = book.find_order(2);
    expect(second && second->remaining == 3, "second order should retain three units");
}

void test_marketable_limit_and_non_crossed_invariant() {
    qdev::OrderBook book;
    book.add_limit(10, qdev::Side::Sell, 101, 10, 1);
    const auto fills = book.add_limit(11, qdev::Side::Buy, 102, 4, 2);
    expect(fills.size() == 1 && fills[0].price_ticks == 101 && fills[0].quantity == 4,
           "crossing limit should execute at resting maker price");
    expect(!book.is_crossed(), "book must not remain crossed");
    expect(book.best_ask() && *book.best_ask() == 101, "residual ask should remain");
    expect(!book.best_bid(), "fully executed crossing buy should not rest");
}

void test_partial_cancel_updates_depth() {
    qdev::OrderBook book;
    book.add_limit(20, qdev::Side::Buy, 99, 10, 1);
    expect(book.cancel(20, 4) == 4, "partial cancel should return cancelled quantity");
    expect(book.best_bid_quantity() == 6, "depth should reflect partial cancel");
    expect(book.cancel(20, 100) == 6, "oversized cancel should remove residual only");
    expect(book.live_order_count() == 0, "fully cancelled order should leave live map");
}

std::vector<qdev::Event> deterministic_events() {
    return {
        {100, 1, qdev::EventType::Add, qdev::Side::Buy, 1, 99, 10},
        {101, 2, qdev::EventType::Add, qdev::Side::Sell, 2, 101, 8},
        {102, 3, qdev::EventType::Market, qdev::Side::Buy, 0, 1, 3},
        {103, 4, qdev::EventType::Cancel, qdev::Side::Buy, 1, 1, 4},
        {104, 5, qdev::EventType::Add, qdev::Side::Buy, 3, 101, 2},
    };
}

void test_deterministic_replay() {
    qdev::EventEngine a;
    qdev::EventEngine b;
    const auto events = deterministic_events();
    a.replay(events);
    b.replay(events);
    expect(a.state_checksum() == b.state_checksum(), "identical input must have identical checksum");
    expect(a.stats().events_processed == events.size(), "engine should process every event");
    expect(!a.book().is_crossed(), "replayed book should be non-crossed");
}

void test_sequence_and_time_guards() {
    qdev::EventEngine engine;
    engine.process({100, 1, qdev::EventType::Add, qdev::Side::Buy, 1, 99, 1});
    bool bad_sequence = false;
    try {
        engine.process({101, 1, qdev::EventType::Market, qdev::Side::Sell, 0, 1, 1});
    } catch (const std::invalid_argument&) { bad_sequence = true; }
    expect(bad_sequence, "duplicate sequence must be rejected");

    bool bad_time = false;
    try {
        engine.process({99, 2, qdev::EventType::Market, qdev::Side::Sell, 0, 1, 1});
    } catch (const std::invalid_argument&) { bad_time = true; }
    expect(bad_time, "timestamp regression must be rejected");
}

void test_quantity_conservation() {
    qdev::OrderBook book;
    book.add_limit(1, qdev::Side::Sell, 101, 10, 1);
    book.add_limit(2, qdev::Side::Sell, 102, 10, 2);
    const auto fills = book.execute_market(qdev::Side::Buy, 13);
    qdev::Quantity executed = 0;
    for (const auto& fill : fills) executed += fill.quantity;
    expect(executed == 13, "market execution should conserve requested quantity when depth exists");
    expect(book.total_resting_quantity() == 7, "resting plus executed should equal initial quantity");
}

void test_price_priority_across_levels() {
    qdev::OrderBook book;
    book.add_limit(1, qdev::Side::Sell, 102, 5, 1);
    book.add_limit(2, qdev::Side::Sell, 101, 5, 2);
    const auto fills = book.execute_market(qdev::Side::Buy, 7);
    expect(fills.size() == 2, "market buy should span two price levels");
    expect(fills[0].maker_order_id == 2 && fills[0].price_ticks == 101 && fills[0].quantity == 5,
           "best ask must execute before worse ask regardless of arrival order");
    expect(fills[1].maker_order_id == 1 && fills[1].price_ticks == 102 && fills[1].quantity == 2,
           "residual should execute at next ask");
}

void test_market_exhaustion_is_safe() {
    qdev::OrderBook book;
    book.add_limit(1, qdev::Side::Sell, 101, 3, 1);
    const auto fills = book.execute_market(qdev::Side::Buy, 10);
    expect(fills.size() == 1 && fills[0].quantity == 3, "market order should consume only available depth");
    expect(book.live_order_count() == 0 && !book.best_ask(), "exhausted side should be empty");
}

void test_order_id_reuse_after_removal_is_safe() {
    qdev::OrderBook book;
    book.add_limit(77, qdev::Side::Buy, 99, 2, 1);
    book.cancel(77, 2);
    book.add_limit(77, qdev::Side::Sell, 101, 3, 2);
    const auto fills = book.execute_market(qdev::Side::Buy, 3);
    expect(fills.size() == 1 && fills[0].maker_order_id == 77 && fills[0].price_ticks == 101,
           "reused ID should refer only to the new order after old order was fully unlinked");
    expect(book.live_order_count() == 0, "reused order should be fully removable without stale FIFO alias");
}

void test_middle_fifo_cancel_is_o1_safe() {
    qdev::OrderBook book;
    book.add_limit(1, qdev::Side::Sell, 101, 5, 1);
    book.add_limit(2, qdev::Side::Sell, 101, 5, 2);
    book.add_limit(3, qdev::Side::Sell, 101, 5, 3);
    expect(book.cancel(2, 5) == 5, "middle order should fully cancel");
    const auto fills = book.execute_market(qdev::Side::Buy, 8);
    expect(fills.size() == 2, "cancelled middle order must be absent from later matching");
    expect(fills[0].maker_order_id == 1 && fills[0].quantity == 5, "head maker should remain first");
    expect(fills[1].maker_order_id == 3 && fills[1].quantity == 3, "tail maker should follow unlinked middle order");
}

void test_checksum_covers_deep_state() {
    qdev::EventEngine a;
    qdev::EventEngine b;
    a.process({100, 1, qdev::EventType::Add, qdev::Side::Buy, 1, 99, 5});
    a.process({101, 2, qdev::EventType::Add, qdev::Side::Buy, 2, 98, 7});
    b.process({100, 1, qdev::EventType::Add, qdev::Side::Buy, 1, 99, 5});
    b.process({101, 2, qdev::EventType::Add, qdev::Side::Buy, 2, 97, 7});
    expect(a.book().best_bid() == b.book().best_bid(), "control books should share top of book");
    expect(a.state_checksum() != b.state_checksum(), "checksum must distinguish different deep-book state");
}

}  // namespace

int main() {
    test_price_time_priority();
    test_marketable_limit_and_non_crossed_invariant();
    test_partial_cancel_updates_depth();
    test_deterministic_replay();
    test_sequence_and_time_guards();
    test_quantity_conservation();
    test_price_priority_across_levels();
    test_market_exhaustion_is_safe();
    test_order_id_reuse_after_removal_is_safe();
    test_middle_fifo_cancel_is_o1_safe();
    test_checksum_covers_deep_state();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all qdev tests passed\n";
    return EXIT_SUCCESS;
}
