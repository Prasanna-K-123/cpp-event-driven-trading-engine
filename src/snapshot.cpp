#include "qdev/engine.hpp"

#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace qdev {
namespace {
constexpr std::array<std::uint8_t, 8> magic{'Q','D','E','V','S','N','A','P'};
constexpr std::size_t max_bytes = 16U * 1024U * 1024U;
constexpr std::size_t header_bytes = 8 + 4 + 1 + 8 + 8 + 7 * 8 + 8;
constexpr std::size_t order_bytes = 8 + 1 + 8 + 8 + 8;

std::uint32_t crc32(const std::vector<std::uint8_t>& bytes, std::size_t n) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xffffffffU;
}

void put(std::vector<std::uint8_t>& out, std::uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 255U));
}

struct Reader {
    const std::vector<std::uint8_t>& data;
    std::size_t pos{};
    std::size_t end{};
    std::uint64_t get(int n) {
        if (static_cast<std::size_t>(n) > end - pos) throw std::invalid_argument("truncated snapshot");
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8U) | data[pos++];
        return v;
    }
};

void check_stats(const EngineStats& s, bool started, std::uint64_t last_sequence,
                 std::uint64_t last_ts, std::uint64_t count) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (s.adds > maximum - s.cancels || s.adds + s.cancels > maximum - s.market_orders ||
        s.events_processed != s.adds + s.cancels + s.market_orders ||
        s.rejected_cancels > s.cancels || s.fills > s.filled_quantity ||
        ((s.fills == 0) != (s.filled_quantity == 0)) || count > s.adds) {
        throw std::invalid_argument("inconsistent snapshot counters");
    }
    if ((s.events_processed == 0 && (started || last_sequence || last_ts || count)) ||
        (s.events_processed != 0 && (!started || last_sequence < s.events_processed - 1))) {
        throw std::invalid_argument("inconsistent snapshot chronology");
    }
}
} // namespace

std::vector<std::uint8_t> EventEngine::snapshot() const {
    const auto orders = book_.resting_orders();
    if (orders.size() > (max_bytes - header_bytes - 4) / order_bytes) {
        throw std::length_error("snapshot exceeds 16 MiB limit");
    }
    std::vector<std::uint8_t> out;
    out.reserve(header_bytes + orders.size() * order_bytes + 4);
    out.insert(out.end(), magic.begin(), magic.end());
    put(out, 1, 4); put(out, started_ ? 1 : 0, 1);
    put(out, last_ts_ns_, 8); put(out, last_sequence_, 8);
    for (const auto v : {stats_.events_processed, stats_.adds, stats_.cancels,
                         stats_.market_orders, stats_.fills, stats_.filled_quantity, stats_.rejected_cancels}) put(out, v, 8);
    put(out, orders.size(), 8);
    for (const auto& o : orders) {
        put(out, o.id, 8); put(out, static_cast<std::uint8_t>(o.side), 1);
        put(out, std::bit_cast<std::uint64_t>(o.price_ticks), 8);
        put(out, std::bit_cast<std::uint64_t>(o.remaining), 8); put(out, o.arrival_sequence, 8);
    }
    put(out, crc32(out, out.size()), 4);
    return out;
}

EventEngine EventEngine::restore(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < header_bytes + 4 || bytes.size() > max_bytes) {
        throw std::invalid_argument("invalid snapshot length");
    }
    Reader trailer{bytes, bytes.size() - 4, bytes.size()};
    if (trailer.get(4) != crc32(bytes, bytes.size() - 4)) throw std::invalid_argument("snapshot CRC mismatch");
    Reader r{bytes, 0, bytes.size() - 4};
    for (const auto ch : magic) if (r.get(1) != ch) throw std::invalid_argument("invalid snapshot magic");
    if (r.get(4) != 1) throw std::invalid_argument("unsupported snapshot version");
    const auto flags = r.get(1);
    if (flags > 1) throw std::invalid_argument("invalid snapshot flags");
    EventEngine result;
    result.started_ = flags == 1;
    result.last_ts_ns_ = r.get(8); result.last_sequence_ = r.get(8);
    auto& s = result.stats_;
    s.events_processed = r.get(8); s.adds = r.get(8); s.cancels = r.get(8);
    s.market_orders = r.get(8); s.fills = r.get(8); s.filled_quantity = r.get(8); s.rejected_cancels = r.get(8);
    const auto count = r.get(8);
    if (count > (r.end - r.pos) / order_bytes || count * order_bytes != r.end - r.pos) {
        throw std::invalid_argument("snapshot order count/trailing bytes mismatch");
    }
    check_stats(s, result.started_, result.last_sequence_, result.last_ts_ns_, count);
    std::unordered_set<OrderId> ids;
    ids.reserve(static_cast<std::size_t>(count));
    result.book_.reserve_order_capacity(static_cast<std::size_t>(count));
    std::uint64_t previous_arrival = 0;
    bool have_previous = false;
    Quantity total_quantity = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto id = r.get(8); const auto side = r.get(1);
        const auto price = std::bit_cast<PriceTicks>(r.get(8));
        const auto quantity = std::bit_cast<Quantity>(r.get(8));
        const auto arrival = r.get(8);
        if (!id || side > 1 || price <= 0 || quantity <= 0 || !ids.insert(id).second ||
            arrival > result.last_sequence_ || (have_previous && arrival <= previous_arrival) ||
            quantity > std::numeric_limits<Quantity>::max() - total_quantity) {
            throw std::invalid_argument("invalid snapshot order/FIFO/quantity");
        }
        total_quantity += quantity;
        previous_arrival = arrival; have_previous = true;
        std::vector<Fill> fills;
        result.book_.add_limit(id, static_cast<Side>(side), price, quantity, arrival, fills);
        if (!fills.empty() || result.book_.is_crossed()) throw std::invalid_argument("crossed snapshot book");
    }
    if (r.pos != r.end) throw std::invalid_argument("snapshot has trailing bytes");
    return result;
}
} // namespace qdev
