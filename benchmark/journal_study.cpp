#include "qdev/journal.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void require(bool x,const char* s){if(!x)throw std::runtime_error(s);}
void hex(std::ostream& out,const std::vector<std::uint8_t>& b) {
    static constexpr char h[]="0123456789abcdef";for(auto x:b)out<<h[x>>4U]<<h[x&15U];
}
struct Temp {
    std::filesystem::path dir;
    Temp(){std::array<char,64> t{};const std::string s="/tmp/qdev-crash-XXXXXX";std::copy(s.begin(),s.end(),t.begin());
        auto* p=::mkdtemp(t.data());if(!p)throw std::runtime_error("mkdtemp");dir=p;}
    ~Temp(){std::error_code ec;std::filesystem::remove_all(dir,ec);}
};
void ready_and_block(int fd) {
    const char c='R';ssize_t n;
    do{n=::write(fd,&c,1);}while(n<0 && errno==EINTR);
    if(n!=1)::_exit(111);
    for(;;)::pause();
}
struct CrashIo:qdev::JournalIo {
    int stage,notify;std::size_t done=0;
    CrashIo(int s,int n):stage(s),notify(n){}
    void checkpoint(qdev::JournalPhase p) override {
        if((stage==0 && p==qdev::JournalPhase::BeforeWrite) ||
           (stage==5 && p==qdev::JournalPhase::AfterWrite) ||
           (stage==6 && p==qdev::JournalPhase::AfterSync) ||
           (stage==7 && p==qdev::JournalPhase::BeforeAck))ready_and_block(notify);
    }
    ssize_t write(int fd,const void* p,std::size_t n) override {
        if(stage>=1 && stage<=4) {
            constexpr std::array<std::size_t,4> offsets{1,7,40,79};
            const auto limit=offsets[static_cast<std::size_t>(stage-1)];
            const auto result=qdev::JournalIo::write(fd,p,std::min(n,limit-done));
            if(result>0)done+=static_cast<std::size_t>(result);
            if(done==limit)ready_and_block(notify);
            return result;
        }
        return qdev::JournalIo::write(fd,p,n);
    }
};
}

int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::invalid_argument("usage: qdev_journal_study OUTPUT_DIRECTORY");
        const std::filesystem::path out=argv[1];std::filesystem::create_directories(out);
        // Refuse overwriting an earlier executed journal/result set.
        if(std::filesystem::exists(out/"summary.json") || std::filesystem::exists(out/"events.csv"))
            throw std::invalid_argument("refuse overwriting previous study outputs");
        std::ofstream inputs(out/"events.csv"),states(out/"states.csv"),crashes(out/"crashes.csv");
        inputs<<"seed,ordinal,ts,sequence,type,side,id,price,quantity\n";
        states<<"seed,ordinal,snapshot_hex\n";
        crashes<<"stage,replicate,notified,killed,recovered_records,tail_bytes,canonical_match\n";
        std::uint64_t total=0,reopens=0;
        for(const std::uint64_t seed:{7,41,2027,20261011}) {
            const auto path=out/("journal_"+std::to_string(seed)+".bin");
            if(std::filesystem::exists(path))throw std::invalid_argument("existing study journal");
            auto j=std::make_unique<qdev::FileJournal>(path);
            qdev::EventEngine uninterrupted;std::mt19937_64 rng(seed);
            for(std::uint64_t i=1;i<=1024;++i) {
                const auto id=rng()%128+1;
                const auto side=rng()%2?qdev::Side::Buy:qdev::Side::Sell;
                const auto qty=1+static_cast<qdev::Quantity>(rng()%100);
                const auto price=90+static_cast<qdev::PriceTicks>(rng()%21);
                const auto choice=rng()%10;
                auto type=qdev::EventType::Add;
                if(choice<2)type=qdev::EventType::Market;
                else if(choice<4 || uninterrupted.book().find_order(id))type=qdev::EventType::Cancel;
                // Journal ordinal is contiguous; feed sequence deliberately has gaps.
                const qdev::Event e{i/3,i*3,type,side,id,price,qty};
                uninterrupted.process(e);require(j->append(e)==i,"ack ordinal");
                require(j->engine().snapshot()==uninterrupted.snapshot(),"filesystem append state mismatch");
                inputs<<seed<<','<<i<<','<<e.ts_ns<<','<<e.sequence<<','<<static_cast<int>(e.type)<<','
                    <<static_cast<int>(e.side)<<','<<e.order_id<<','<<e.price_ticks<<','<<e.quantity<<'\n';
                ++total;
                if(i%64==0) {
                    j.reset();j=std::make_unique<qdev::FileJournal>(path);++reopens;
                    require(j->records()==i && j->engine().snapshot()==uninterrupted.snapshot(),"filesystem reopen state mismatch");
                    states<<seed<<','<<i<<',';hex(states,j->engine().snapshot());states<<'\n';
                }
            }
        }
        // A pipe proves the child reached the requested boundary before SIGKILL.
        // Killing a process leaves the OS/cache alive; this is NOT a power cut.
        const std::vector<qdev::Event> base{
            {100,10,qdev::EventType::Add,qdev::Side::Sell,1,101,5},
            {100,20,qdev::EventType::Add,qdev::Side::Sell,2,101,7},
            {101,30,qdev::EventType::Market,qdev::Side::Buy,0,0,6}};
        std::uint64_t crash_count=0;
        for(int stage=0;stage<9;++stage)for(int rep=0;rep<32;++rep) {
            Temp temp;const auto path=temp.dir/"events.bin";
            qdev::EventEngine expected;{qdev::FileJournal j(path);for(const auto& e:base){j.append(e);expected.process(e);}}
            const qdev::Event next{102,40,qdev::EventType::Add,qdev::Side::Buy,3,100,1+rep};
            int pipes[2];require(::pipe(pipes)==0,"pipe failed");
            const auto pid=::fork();require(pid>=0,"fork failed");
            if(pid==0) {
                ::close(pipes[0]);
                try {auto io=std::make_shared<CrashIo>(stage,pipes[1]);qdev::FileJournal j(path,io);j.append(next);
                    if(stage==8)ready_and_block(pipes[1]);
                    ::_exit(112);}
                catch(...){::_exit(113);}
            }
            ::close(pipes[1]);struct pollfd pfd{pipes[0],POLLIN,0};int result;
            do{result=::poll(&pfd,1,10000);}while(result<0 && errno==EINTR);
            char signal=0;const auto notified=result>0 && ::read(pipes[0],&signal,1)==1 && signal=='R';
            ::close(pipes[0]);const auto killed=::kill(pid,SIGKILL)==0;int status=0;
            while(::waitpid(pid,&status,0)<0){if(errno!=EINTR)throw std::runtime_error("waitpid failed");}
            require(notified && killed && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL,"child did not reach/stop at registered boundary");
            if(stage>=5)expected.process(next);
            qdev::FileJournal recovered(path);
            const auto expected_count=stage>=5?4U:3U;
            constexpr std::array<std::size_t,4> offsets{1,7,40,79};
            const auto tail=stage>=1 && stage<=4?offsets[static_cast<std::size_t>(stage-1)]:0;
            require(recovered.records()==expected_count && recovered.recovered_tail_bytes()==tail &&
                recovered.engine().snapshot()==expected.snapshot(),"process-crash recovery mismatch");
            crashes<<stage<<','<<rep<<",1,1,"<<recovered.records()<<','<<recovered.recovered_tail_bytes()<<",1\n";++crash_count;
        }
        // A fork-inherited handle is rejected; a separate child open respects the lock.
        std::uint64_t fork_guards=0;
        {Temp temp;qdev::FileJournal parent(temp.dir/"guard.bin");
            for(int kind=0;kind<2;++kind){const auto pid=::fork();require(pid>=0,"guard fork");
                if(pid==0){try{if(kind==0)parent.append({1,1,qdev::EventType::Market,qdev::Side::Buy,0,0,1});
                    else {qdev::FileJournal child(temp.dir/"guard.bin");}::_exit(1);}catch(...){::_exit(0);}}
                int status;while(::waitpid(pid,&status,0)<0){if(errno!=EINTR)throw std::runtime_error("guard wait");}
                require(WIFEXITED(status) && WEXITSTATUS(status)==0,"fork/lock guard failed");++fork_guards;}}
        inputs.close();states.close();crashes.close();require(inputs && states && crashes,"study output write failure");
        struct statfs fs{};require(::statfs(out.c_str(),&fs)==0,"statfs");
        std::ofstream summary(out/"summary.json");
        summary<<"{\n  \"events\": "<<total<<",\n  \"seeds\": [7,41,2027,20261011],\n  \"reopens\": "<<reopens
            <<",\n  \"process_crash_cases\": "<<crash_count<<",\n  \"crash_stages\": 9,\n  \"replicates_per_stage\": 32,\n  \"fork_guards\": "<<fork_guards
            <<",\n  \"filesystem_magic_decimal\": "<<fs.f_type<<",\n  \"record_bytes\": 80,\n  \"mismatches\": 0,\n  \"actual_power_loss_tested\": false\n}\n";
        summary.close();require(static_cast<bool>(summary),"summary write failed");
        std::cout<<"JOURNAL_STUDY_PASS events="<<total<<" reopens="<<reopens<<" process_crash_cases="<<crash_count<<" fork_guards="<<fork_guards<<"\n";
        return EXIT_SUCCESS;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
}
