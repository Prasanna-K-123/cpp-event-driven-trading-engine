// Differential verification against an intentionally slow vector-based oracle.
// The oracle shares no matching/indexing implementation with the production book.
#include "qdev/order_book.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
struct Order { qdev::OrderId id; qdev::Side side; qdev::PriceTicks price; qdev::Quantity qty; std::uint64_t seq; };
class Oracle {
public:
    std::vector<Order> orders;
    std::vector<qdev::Fill> match(qdev::Side side,qdev::Quantity qty,std::optional<qdev::PriceTicks> limit={}) {
        std::vector<qdev::Fill> fills;
        while(qty>0) {
            std::size_t best=orders.size();
            for(std::size_t k=0;k<orders.size();++k) {
                const auto& candidate=orders[k];
                if(candidate.side==side) continue;
                if(limit && ((side==qdev::Side::Buy && candidate.price>*limit) ||
                             (side==qdev::Side::Sell && candidate.price<*limit))) continue;
                if(best==orders.size() ||
                   (side==qdev::Side::Buy ? candidate.price<orders[best].price : candidate.price>orders[best].price) ||
                   (candidate.price==orders[best].price && candidate.seq<orders[best].seq)) best=k;
            }
            if(best==orders.size()) break;
            auto& maker=orders[best];
            const auto take=std::min(qty,maker.qty);
            fills.push_back({maker.id,maker.side,maker.price,take});
            maker.qty-=take;
            qty-=take;
            if(maker.qty==0) orders.erase(orders.begin()+static_cast<std::ptrdiff_t>(best));
        }
        return fills;
    }
    std::vector<qdev::Fill> add(qdev::OrderId id,qdev::Side side,qdev::PriceTicks price,qdev::Quantity qty,std::uint64_t seq) {
        auto fills=match(side,qty,price);
        for(const auto& f:fills) qty-=f.quantity;
        if(qty>0) orders.push_back({id,side,price,qty,seq});
        return fills;
    }
    qdev::Quantity cancel(qdev::OrderId id,qdev::Quantity qty) {
        auto it=std::find_if(orders.begin(),orders.end(),[id](const auto& o){return o.id==id;});
        if(it==orders.end()) return 0;
        const auto removed=std::min(qty,it->qty);
        it->qty-=removed;
        if(it->qty==0) orders.erase(it);
        return removed;
    }
};

void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void compare_fills(const std::vector<qdev::Fill>& a,const std::vector<qdev::Fill>& b) {
    require(a.size()==b.size(),"fill count differs from independent oracle");
    for(std::size_t k=0;k<a.size();++k) require(
        a[k].maker_order_id==b[k].maker_order_id && a[k].maker_side==b[k].maker_side &&
        a[k].price_ticks==b[k].price_ticks && a[k].quantity==b[k].quantity,"maker/fill priority differs");
}
void compare_state(const qdev::OrderBook& book,const Oracle& oracle) {
    qdev::Quantity total=0,bid_qty=0,ask_qty=0;
    std::optional<qdev::PriceTicks> bid,ask;
    for(const auto& o:oracle.orders) {
        total+=o.qty;
        if(o.side==qdev::Side::Buy && (!bid || o.price>*bid)) bid=o.price;
        if(o.side==qdev::Side::Sell && (!ask || o.price<*ask)) ask=o.price;
    }
    for(const auto& o:oracle.orders) {
        if(o.side==qdev::Side::Buy && bid && o.price==*bid) bid_qty+=o.qty;
        if(o.side==qdev::Side::Sell && ask && o.price==*ask) ask_qty+=o.qty;
    }
    require(book.best_bid()==bid && book.best_ask()==ask,"best prices differ");
    require(book.best_bid_quantity()==bid_qty && book.best_ask_quantity()==ask_qty,"level quantities differ");
    require(book.total_resting_quantity()==total && book.live_order_count()==oracle.orders.size(),"aggregate conservation differs");
    require(!book.is_crossed(),"crossed book after matching");
    for(qdev::OrderId id=1;id<=256;++id) {
        const auto* actual=book.find_order(id);
        const auto expected=std::find_if(oracle.orders.begin(),oracle.orders.end(),[id](const auto& o){return o.id==id;});
        require((actual==nullptr)==(expected==oracle.orders.end()),"live-order index differs");
        if(actual) require(actual->side==expected->side && actual->price_ticks==expected->price &&
                           actual->remaining==expected->qty && actual->arrival_sequence==expected->seq,"deep order state differs");
    }
}
}

int main() {
    try {
        for(const std::uint64_t seed:{7,41,2027,20261009,8675309}) {
            qdev::OrderBook book;
            Oracle oracle;
            std::mt19937_64 rng(seed);
            for(std::uint64_t seq=1;seq<=5000;++seq) {
                const qdev::OrderId id=rng()%256+1;
                const auto side=rng()%2 ? qdev::Side::Buy : qdev::Side::Sell;
                const qdev::PriceTicks price=90+static_cast<qdev::PriceTicks>(rng()%21);
                const qdev::Quantity qty=1+static_cast<qdev::Quantity>(rng()%100);
                const auto choice=rng()%10;
                if(choice<2) compare_fills(book.execute_market(side,qty),oracle.match(side,qty));
                else if(choice<4 || book.find_order(id)) require(book.cancel(id,qty)==oracle.cancel(id,qty),"cancel differs");
                else compare_fills(book.add_limit(id,side,price,qty,seq),oracle.add(id,side,price,qty,seq));
                compare_state(book,oracle);
            }
        }
        std::cout << "25,000 events across 5 seeds matched the independent oracle, including all 256 ID slots after every event\n";
        return EXIT_SUCCESS;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
