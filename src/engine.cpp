#include "qdev/engine.hpp"

#include <stdexcept>

namespace qdev {

namespace {

std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
    return h;
}

}  // namespace

void EventEngine::account_fills(const std::vector<Fill>& fills) {
    stats_.fills += fills.size();
    for (const auto& fill : fills) {
        stats_.filled_quantity += static_cast<std::uint64_t>(fill.quantity);
    }
}

void EventEngine::process(const Event& event) {
    validate_event(event);
    if (started_) {
        if (event.ts_ns < last_ts_ns_) {
            throw std::invalid_argument("timestamps must be nondecreasing");
        }
        if (event.sequence <= last_sequence_) {
            throw std::invalid_argument("sequence must be strictly increasing");
        }
    }

    fill_scratch_.clear();
    switch (event.type) {
        case EventType::Add: {
            book_.add_limit(event.order_id, event.side, event.price_ticks,
                            event.quantity, event.sequence, fill_scratch_);
            account_fills(fill_scratch_);
            ++stats_.adds;
            break;
        }
        case EventType::Cancel: {
            const Quantity cancelled = book_.cancel(event.order_id, event.quantity);
            if (cancelled == 0) ++stats_.rejected_cancels;
            ++stats_.cancels;
            break;
        }
        case EventType::Market: {
            book_.execute_market(event.side, event.quantity, fill_scratch_);
            account_fills(fill_scratch_);
            ++stats_.market_orders;
            break;
        }
    }

#ifndef NDEBUG
    if (book_.is_crossed()) {
        throw std::logic_error("order book crossed after event processing");
    }
#endif
    last_ts_ns_ = event.ts_ns;
    last_sequence_ = event.sequence;
    started_ = true;
    ++stats_.events_processed;
}

void EventEngine::replay(const std::vector<Event>& events) {
    for (const auto& event : events) process(event);
}

std::uint64_t EventEngine::state_checksum() const noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    h = mix(h, stats_.events_processed);
    h = mix(h, stats_.fills);
    h = mix(h, stats_.filled_quantity);
    h = mix(h, stats_.rejected_cancels);
    h = mix(h, last_ts_ns_);
    h = mix(h, last_sequence_);
    try {
        h = mix(h, book_.state_checksum());
    } catch (...) {
        return 0;
    }
    return h;
}

}  // namespace qdev
