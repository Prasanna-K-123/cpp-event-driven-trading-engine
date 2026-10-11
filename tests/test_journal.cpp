#include "qdev/journal.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void require(bool x,const char* m) { if(!x) throw std::runtime_error(m); }
template<class F> void refused(F f) {
    bool failed=false;try{f();}catch(const std::exception&){failed=true;}
    require(failed,"expected refusal");
}
struct Temp {
    std::filesystem::path dir;
    Temp(){std::array<char,64> t{};const std::string s="/tmp/qdev-journal-XXXXXX";std::copy(s.begin(),s.end(),t.begin());
        const auto* p=::mkdtemp(t.data());if(!p)throw std::runtime_error("mkdtemp");dir=p;}
    ~Temp(){std::error_code ec;std::filesystem::remove_all(dir,ec);}
};
std::vector<std::uint8_t> read(const std::filesystem::path& p) {
    std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};
}
void write(const std::filesystem::path& p,const std::vector<std::uint8_t>& b) {
    std::ofstream f(p,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));
    if(!f)throw std::runtime_error("fixture write failed");
}
const std::vector<qdev::Event> events{
    {100,10,qdev::EventType::Add,qdev::Side::Sell,1,101,5},
    {100,20,qdev::EventType::Add,qdev::Side::Sell,2,101,7},
    {101,30,qdev::EventType::Market,qdev::Side::Buy,0,0,6}};
void reseal(std::vector<std::uint8_t>& b) {
    std::uint32_t previous=0;
    for(std::size_t start=0;start<b.size();start+=80) {
        for(int i=0;i<4;++i)b[start+72+i]=static_cast<std::uint8_t>(previous>>(8*(3-i)));
        std::uint32_t crc=0xffffffffU;
        for(std::size_t i=start;i<start+76;++i) {crc^=b[i];for(int k=0;k<8;++k)crc=(crc>>1U)^(0xedb88320U&(0U-(crc&1U)));}
        crc^=0xffffffffU;for(int i=0;i<4;++i)b[start+76+i]=static_cast<std::uint8_t>(crc>>(8*(3-i)));
        previous=crc;
    }
}
struct FaultIo: qdev::JournalIo {
    std::size_t write_limit=80,read_limit=80,done=0;
    int write_eintr=0,read_eintr=0,sync_eintr=0,truncate_eintr=0;
    std::size_t stop_after=std::numeric_limits<std::size_t>::max();
    int write_error=ENOSPC;bool zero=false,read_error=false,truncate_error=false;
    int sync_calls=0,sync_fail_call=0;
    ssize_t write(int fd,const void* p,std::size_t n) override {
        if(write_eintr-->0){errno=EINTR;return -1;}
        if(done>=stop_after){errno=write_error;return zero?0:-1;}
        const auto actual=std::min({n,write_limit,stop_after-done});
        const auto result=qdev::JournalIo::write(fd,p,actual);if(result>0)done+=static_cast<std::size_t>(result);return result;
    }
    ssize_t read(int fd,void* p,std::size_t n,off_t offset) override {
        if(read_eintr-->0){errno=EINTR;return -1;}
        if(read_error){errno=EIO;return -1;}
        return qdev::JournalIo::read(fd,p,std::min(n,read_limit),offset);
    }
    int sync(int fd) override {
        if(sync_eintr-->0){errno=EINTR;return -1;}
        if(++sync_calls==sync_fail_call){errno=EIO;return -1;}
        return qdev::JournalIo::sync(fd);
    }
    int truncate(int fd,off_t n) override {
        if(truncate_eintr-->0){errno=EINTR;return -1;}
        if(truncate_error){errno=EIO;return -1;}
        return qdev::JournalIo::truncate(fd,n);
    }
};
}

int main(int argc,char** argv) {
    try {
        Temp tmp;const auto p=tmp.dir/"journal.bin";
        qdev::EventEngine expected;
        {qdev::FileJournal j(p);for(const auto& e:events){expected.process(e);j.append(e);require(j.engine().snapshot()==expected.snapshot(),"append state");}}
        const auto bytes=read(p);require(bytes.size()==240,"fixture size");
        if(argc==2 && std::string(argv[1])=="--fixture") {
            static constexpr char hex[]="0123456789abcdef";
            std::cout<<"JOURNAL_FIXTURE_HEX=";for(auto b:bytes)std::cout<<hex[b>>4U]<<hex[b&15U];std::cout<<'\n';return 0;
        }
        // Empty/missing/complete files, cooperative exclusive lock, FIFO after reopen.
        {qdev::FileJournal j(p);require(j.records()==3 && j.engine().snapshot()==expected.snapshot(),"recovery state");
            refused([&]{qdev::FileJournal other(p);});
            const auto before=read(p);const auto image=j.engine().snapshot();
            for(const auto e:std::vector<qdev::Event>{
                {101,30,qdev::EventType::Market,qdev::Side::Buy,0,0,1},
                {99,31,qdev::EventType::Market,qdev::Side::Buy,0,0,1},
                {101,31,qdev::EventType::Add,qdev::Side::Sell,2,101,1},
                {101,31,qdev::EventType::Market,qdev::Side::Buy,0,0,0},
                {101,31,static_cast<qdev::EventType>(255),qdev::Side::Buy,0,0,1},
                {101,31,qdev::EventType::Market,static_cast<qdev::Side>(255),0,0,1},
                {101,31,qdev::EventType::Market,qdev::Side::Buy,0,0,qdev::FileJournal::max_event_quantity+1},
                {101,31,qdev::EventType::Market,qdev::Side::Buy,0,0,std::numeric_limits<qdev::Quantity>::max()}})
                refused([&]{j.append(e);});
            require(read(p)==before && j.engine().snapshot()==image && !j.poisoned(),"invalid event changed committed state");
            j.append({102,40,qdev::EventType::Market,qdev::Side::Buy,0,0,2});
            require(j.engine().book().find_order(2)->remaining==4,"recovered FIFO");}
        std::uint64_t bits=0,prefixes=0;
        for(std::size_t i=0;i<bytes.size();++i)for(int bit=0;bit<8;++bit) {
            auto b=bytes;b[i]^=1U<<bit;write(p,b);refused([&]{qdev::FileJournal j(p);});
            require(read(p)==b,"corrupt full record changed on refusal");++bits;}
        for(std::size_t n=0;n<bytes.size();++n) {
            write(p,{bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(n)});
            qdev::EventEngine prefix;for(std::size_t i=0;i<n/80;++i)prefix.process(events[i]);
            {qdev::FileJournal j(p);require(j.records()==n/80 && j.recovered_tail_bytes()==n%80 && j.engine().snapshot()==prefix.snapshot(),"partial prefix recovery");
                j.append(events[n/80]);prefix.process(events[n/80]);require(j.engine().snapshot()==prefix.snapshot(),"append after trim");}
            {qdev::FileJournal j(p);require(j.engine().snapshot()==prefix.snapshot(),"trim/reappend recovery");}++prefixes;
        }
        // Malformed records with recomputed valid CRCs still fail structure or engine rules.
        std::uint64_t forged=0;
        for(int kind=0;kind<13;++kind) {
            auto b=bytes;
            switch(kind) {
                case 0:b[0]='X';break;
                case 1:b[11]=2;break;
                case 2:b[15]=79;break;
                case 3:b[23]=2;break;
                case 4:b[40]=255;break;
                case 5:b[41]=255;break;
                case 6:b[42]=1;break;
                case 7:b[55]=0;break;
                case 8:std::fill(b.begin()+56,b.begin()+64,0);break;
                case 9:std::fill(b.begin()+64,b.begin()+72,0);break;
                case 10:b[80+39]=10;break;
                case 11:b[80+31]=99;break;
                case 12:std::fill(b.begin()+64,b.begin()+72,255);b[64]=127;break;
            }
            reseal(b);write(p,b);refused([&]{qdev::FileJournal j(p);});require(read(p)==b,"forged record modified");++forged;
        }
        std::uint64_t chain_cases=0;
        for(int kind=0;kind<3;++kind) {
            auto b=bytes;
            if(kind==0)std::swap_ranges(b.begin(),b.begin()+80,b.begin()+80);
            if(kind==1)b.erase(b.begin()+80,b.begin()+160);
            if(kind==2)b.insert(b.begin()+80,bytes.begin(),bytes.begin()+80);
            write(p,b);refused([&]{qdev::FileJournal j(p);});++chain_cases;
        }
        std::uint64_t faults=0;
        for(const auto stop:std::array<std::size_t,6>{0,1,40,79,0,40}) {
            const bool zero=faults>=4;write(p,{bytes.begin(),bytes.begin()+80});
            auto io=std::make_shared<FaultIo>();io->stop_after=stop;io->zero=zero;
            {qdev::FileJournal j(p,io);const auto image=j.engine().snapshot();refused([&]{j.append(events[1]);});
                require(j.poisoned() && j.records()==1 && j.engine().snapshot()==image,"I/O failure acknowledged/mutated");
                refused([&]{j.append(events[1]);});}
            {qdev::FileJournal j(p);require(j.records()==1 && j.recovered_tail_bytes()==stop,"fault partial recovery");}
            ++faults;
        }
        write(p,{bytes.begin(),bytes.begin()+80});
        {auto io=std::make_shared<FaultIo>();io->write_limit=7;io->write_eintr=3;io->sync_eintr=3;
            qdev::FileJournal j(p,io);io->sync_eintr=3;j.append(events[1]);require(j.records()==2,"short/EINTR write");++faults;}
        {auto io=std::make_shared<FaultIo>();io->read_limit=7;io->read_eintr=3;qdev::FileJournal j(p,io);require(j.records()==2,"short/EINTR read");++faults;}
        write(p,{bytes.begin(),bytes.begin()+80});
        {auto io=std::make_shared<FaultIo>();qdev::FileJournal j(p,io);io->sync_fail_call=io->sync_calls+1;
            const auto image=j.engine().snapshot();refused([&]{j.append(events[1]);});
            require(j.poisoned() && j.records()==1 && j.engine().snapshot()==image,"failed sync acknowledged");++faults;}
        {qdev::FileJournal j(p);require(j.records()==2,"complete uncertain record must recover locally");}
        for(int kind=0;kind<5;++kind) {
            write(p,{bytes.begin(),bytes.begin()+81});auto io=std::make_shared<FaultIo>();
            if(kind==0)io->truncate_error=true;
            if(kind==1)io->sync_fail_call=1;
            if(kind==2)io->sync_fail_call=2;
            if(kind==3)io->read_error=true;
            if(kind==4)io->truncate_eintr=3;
            if(kind==4){qdev::FileJournal j(p,io);require(j.records()==1,"truncate EINTR");}
            else refused([&]{qdev::FileJournal j(p,io);});
            ++faults;
        }
        // No unbounded allocation or traversal of final symlink/nonregular file.
        write(p,bytes);const auto link=tmp.dir/"link";std::filesystem::create_symlink(p,link);refused([&]{qdev::FileJournal j(link);});
        const auto fifo=tmp.dir/"fifo";require(::mkfifo(fifo.c_str(),0600)==0,"mkfifo");refused([&]{qdev::FileJournal j(fifo);});
        std::filesystem::resize_file(p,qdev::FileJournal::max_records*80+1);refused([&]{qdev::FileJournal j(p);});
        refused([&]{qdev::FileJournal j(tmp.dir/"missing"/"x");});
        refused([&]{qdev::FileJournal j(tmp.dir/"x",nullptr);});
        std::cout<<"JOURNAL_UNIT_JSON={\"single_bit_refusals\":"<<bits<<",\"truncation_prefixes\":"<<prefixes
            <<",\"resealed_invalid_records\":"<<forged<<",\"splice_refusals\":"<<chain_cases<<",\"io_fault_scenarios\":"<<faults
            <<",\"invalid_append_scenarios\":8,\"path_and_size_scenarios\":5,\"exclusive_lock\":true}\n";
        return EXIT_SUCCESS;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
}
