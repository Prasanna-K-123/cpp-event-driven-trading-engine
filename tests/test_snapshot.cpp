#include "qdev/engine.hpp"
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
void require(bool yes, const char* message) { if (!yes) throw std::runtime_error(message); }
void same(const qdev::EventEngine& a, const qdev::EventEngine& b) {
    require(a.state_checksum() == b.state_checksum(), "checksum differs after recovery");
    const auto x=a.snapshot(), y=b.snapshot();
    require(x==y, "canonical image differs after recovery");
    for (qdev::OrderId id=1;id<=256;++id) {
        const auto *p=a.book().find_order(id), *q=b.book().find_order(id);
        require((p==nullptr)==(q==nullptr), "live index differs");
        if(p) require(p->side==q->side && p->price_ticks==q->price_ticks && p->remaining==q->remaining &&
            p->arrival_sequence==q->arrival_sequence && p->prev_id==q->prev_id && p->next_id==q->next_id, "deep FIFO state differs");
    }
}
void reseal(std::vector<std::uint8_t>& data) {
    std::uint32_t crc=0xffffffffU;
    for(std::size_t i=0;i<data.size()-4;++i) {
        crc^=data[i];
        for(int k=0;k<8;++k) crc=(crc>>1U)^(0xedb88320U&(0U-(crc&1U)));
    }
    crc^=0xffffffffU;
    for(int i=0;i<4;++i) data[data.size()-4+i]=static_cast<std::uint8_t>(crc>>(8*(3-i)));
}
void refused(const std::vector<std::uint8_t>& bytes) {
    bool caught=false;
    try { auto ignored=qdev::EventEngine::restore(bytes); (void)ignored; }
    catch(const std::invalid_argument&) {caught=true;}
    require(caught, "malformed recovery image was accepted");
}
}

int main() {
    try {
        qdev::EventEngine empty;
        auto recovered_empty=qdev::EventEngine::restore(empty.snapshot());same(empty,recovered_empty);
        std::uint64_t recoveries=0;
        for(const std::uint64_t seed:{7,41,2027,20261010,8675309}) {
            qdev::EventEngine uninterrupted,recovered;
            std::mt19937_64 rng(seed);
            for(std::uint64_t seq=1;seq<=5000;++seq) {
                const auto id=rng()%256+1;
                const auto side=rng()%2?qdev::Side::Buy:qdev::Side::Sell;
                const auto qty=1+static_cast<qdev::Quantity>(rng()%100);
                const auto price=90+static_cast<qdev::PriceTicks>(rng()%21);
                const auto choice=rng()%10;
                auto type=qdev::EventType::Add;
                if(choice<2)type=qdev::EventType::Market;
                else if(choice<4 || uninterrupted.book().find_order(id))type=qdev::EventType::Cancel;
                const qdev::Event event{seq/3,seq,type,side,id,price,qty};
                uninterrupted.process(event);recovered.process(event);
                if(seq%37==0 || seq==1 || seq==5000) {recovered=qdev::EventEngine::restore(recovered.snapshot());++recoveries;}
                same(uninterrupted,recovered);
            }
        }
        qdev::EventEngine example;
        example.process({100,1,qdev::EventType::Add,qdev::Side::Sell,1,101,5});
        example.process({100,2,qdev::EventType::Add,qdev::Side::Sell,2,101,7});
        const auto bytes=example.snapshot();
        // Every single-bit flip in this complete image must fail CRC checking.
        std::uint64_t corruptions=0;
        for(std::size_t i=0;i<bytes.size();++i)for(int bit=0;bit<8;++bit){auto x=bytes;x[i]^=1U<<bit;refused(x);++corruptions;}
        for(std::size_t n=0;n<bytes.size();++n)refused({bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(n)});
        auto changed=bytes;changed[11]=2;reseal(changed);refused(changed); // version
        changed=bytes;changed[12]=2;reseal(changed);refused(changed); // flag
        changed=bytes;changed[92]=3;reseal(changed);refused(changed); // impossible count
        changed=bytes;changed[93+8]=9;reseal(changed);refused(changed); // side
        changed=bytes;changed[93+33+7]=1;reseal(changed);refused(changed); // duplicate ID
        changed=bytes;changed[93+33+32]=1;reseal(changed);refused(changed); // repeated arrival
        changed=bytes;for(std::size_t i=93+9;i<93+17;++i)changed[i]=0;reseal(changed);refused(changed); // zero price
        changed=bytes;for(std::size_t i=93+17;i<93+25;++i)changed[i]=0;reseal(changed);refused(changed); // zero qty
        auto restored=qdev::EventEngine::restore(bytes);
        auto checkpoint=restored.snapshot();
        try{restored.process({100,2,qdev::EventType::Market,qdev::Side::Buy,0,0,1});require(false,"duplicate sequence accepted after restore");}catch(const std::invalid_argument&){}
        require(checkpoint==restored.snapshot(),"failed event mutated restored engine");
        try{restored.process({99,3,qdev::EventType::Market,qdev::Side::Buy,0,0,1});require(false,"time regression accepted after restore");}catch(const std::invalid_argument&){}
        restored.process({101,3,qdev::EventType::Market,qdev::Side::Buy,0,0,6});
        require(restored.book().find_order(1)==nullptr && restored.book().find_order(2)->remaining==6,"FIFO execution changed on recovery");
        for(const auto type:{static_cast<qdev::EventType>(255)})try{restored.process({101,4,type,qdev::Side::Buy,0,0,1});require(false,"invalid enum accepted");}catch(const std::invalid_argument&){}
        try{restored.process({101,4,qdev::EventType::Market,static_cast<qdev::Side>(255),0,0,1});require(false,"invalid side accepted");}catch(const std::invalid_argument&){}
        std::cout<<"SNAPSHOT_RECOVERY_PASS events=25000 seeds=5 restorations="<<recoveries<<" single_bit_corruptions="<<corruptions<<" truncations="<<bytes.size()<<" forged_structure_cases=8\n";
        std::cout<<"SNAPSHOT_FIXTURE_HEX=";
        for(const auto byte:bytes)std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned int>(byte);
        std::cout<<'\n';
        return EXIT_SUCCESS;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
}
