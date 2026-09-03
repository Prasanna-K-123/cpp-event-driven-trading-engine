#include "qdev/engine.hpp"
#include "qdev/order_book.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void test_caller_owned_fill_buffer_reuse() {
    qdev::OrderBook book;
    static_cast<void>(book.add_limit(1, qdev::Side::Sell, 101, 5, 1));

    std::vector<qdev::Fill> fills;
    fills.reserve(8);
    fills.push_back({999, qdev::Side::Buy, 1, 1});
    const auto* storage = fills.data();

    book.add_limit(2, qdev::Side::Buy, 101, 3, 2, fills);
    expect(fills.size() == 1, "caller-owned limit buffer should contain only current fills");
    expect(fills[0].maker_order_id == 1 && fills[0].quantity == 3,
           "marketable limit should report the expected maker fill");
    expect(fills.data() == storage, "limit matching should reuse sufficient caller-owned capacity");

    book.execute_market(qdev::Side::Buy, 2, fills);
    expect(fills.size() == 1 && fills[0].maker_order_id == 1 && fills[0].quantity == 2,
           "market execution should replace prior fill contents");
    expect(fills.data() == storage, "market matching should reuse sufficient caller-owned capacity");
}

void test_engine_reuses_path_without_changing_accounting() {
    qdev::EventEngine engine;
    const std::vector<qdev::Event> events{
        {1, 1, qdev::EventType::Add, qdev::Side::Sell, 10, 101, 5},
        {2, 2, qdev::EventType::Market, qdev::Side::Buy, 0, 1, 3},
        {3, 3, qdev::EventType::Cancel, qdev::Side::Sell, 10, 1, 1},
        {4, 4, qdev::EventType::Market, qdev::Side::Buy, 0, 1, 1},
    };
    engine.replay(events);

    expect(engine.stats().events_processed == 4, "engine should process every event");
    expect(engine.stats().fills == 2, "fill count should remain exact across buffer reuse");
    expect(engine.stats().filled_quantity == 4, "filled quantity should remain exact across buffer reuse");
    expect(engine.book().live_order_count() == 0, "final book state should be unchanged by buffer reuse");
}

}  // namespace

int main() {
    test_caller_owned_fill_buffer_reuse();
    test_engine_reuses_path_without_changing_accounting();
    if (failures != 0) {
        std::cerr << failures << " fill-buffer test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "fill-buffer reuse tests passed\n";
    return EXIT_SUCCESS;
}
