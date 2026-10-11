#include "qdev/journal.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <type_traits>
#include <unistd.h>

namespace qdev {
namespace {
using Record = std::array<std::uint8_t, FileJournal::record_bytes>;
constexpr std::array<std::uint8_t,8> magic{'Q','D','E','V','R','E','C','1'};
[[noreturn]] void io_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
void put(Record& r, std::size_t p, std::uint64_t v, std::size_t n) {
    for (std::size_t i=0;i<n;++i) r[p+i]=static_cast<std::uint8_t>(v>>(8*(n-1-i)));
}
std::uint64_t get(const Record& r, std::size_t p, std::size_t n) {
    std::uint64_t v=0;
    for(std::size_t i=0;i<n;++i) v=(v<<8U)|r[p+i];
    return v;
}
std::uint32_t crc(const Record& r) {
    std::uint32_t value=0xffffffffU;
    for(std::size_t i=0;i<76;++i) {
        value^=r[i];
        for(int k=0;k<8;++k) value=(value>>1U)^(0xedb88320U&(0U-(value&1U)));
    }
    return value^0xffffffffU;
}
Record encode(const Event& e, std::uint64_t index, std::uint32_t previous) {
    Record r{};
    std::copy(magic.begin(),magic.end(),r.begin());
    put(r,8,1,4); put(r,12,r.size(),4); put(r,16,index,8);
    put(r,24,e.ts_ns,8); put(r,32,e.sequence,8);
    r[40]=static_cast<std::uint8_t>(e.type); r[41]=static_cast<std::uint8_t>(e.side);
    put(r,48,e.order_id,8);
    put(r,56,std::bit_cast<std::uint64_t>(e.price_ticks),8);
    put(r,64,std::bit_cast<std::uint64_t>(e.quantity),8);
    put(r,72,previous,4); put(r,76,crc(r),4);
    return r;
}
Event decode(const Record& r, std::uint64_t index, std::uint32_t previous) {
    if(!std::equal(magic.begin(),magic.end(),r.begin()) || get(r,8,4)!=1 ||
       get(r,12,4)!=r.size() || get(r,16,8)!=index ||
       get(r,72,4)!=previous || get(r,76,4)!=crc(r))
        throw std::invalid_argument("invalid journal frame/chain/CRC");
    for(std::size_t i=42;i<48;++i) if(r[i]!=0)
        throw std::invalid_argument("nonzero journal reserved field");
    return {get(r,24,8),get(r,32,8),static_cast<EventType>(r[40]),
            static_cast<Side>(r[41]),get(r,48,8),
            std::bit_cast<std::int64_t>(get(r,56,8)),std::bit_cast<std::int64_t>(get(r,64,8))};
}
}

ssize_t JournalIo::write(int fd,const void* bytes,std::size_t count) { return ::write(fd,bytes,count); }
ssize_t JournalIo::read(int fd,void* bytes,std::size_t count,off_t offset) { return ::pread(fd,bytes,count,offset); }
int JournalIo::sync(int fd) { return ::fsync(fd); }
int JournalIo::truncate(int fd,off_t length) { return ::ftruncate(fd,length); }

void FileJournal::sync_checked(int fd) {
    while(io_->sync(fd)!=0) { if(errno!=EINTR) io_error("journal fsync"); }
}
void FileJournal::read_exact(void* bytes,std::size_t count,off_t offset) {
    auto* p=static_cast<std::uint8_t*>(bytes);
    std::size_t done=0;
    while(done<count) {
        const auto n=io_->read(fd_,p+done,count-done,offset+static_cast<off_t>(done));
        if(n<0) { if(errno==EINTR) continue; io_error("journal pread"); }
        if(n==0 || static_cast<std::size_t>(n)>count-done)
            throw std::runtime_error("journal read returned unexpected length");
        done+=static_cast<std::size_t>(n);
    }
}
void FileJournal::write_exact(const void* bytes,std::size_t count) {
    const auto* p=static_cast<const std::uint8_t*>(bytes);
    std::size_t done=0;
    while(done<count) {
        const auto n=io_->write(fd_,p+done,count-done);
        if(n<0) { if(errno==EINTR) continue; io_error("journal write"); }
        if(n==0 || static_cast<std::size_t>(n)>count-done)
            throw std::runtime_error("journal write returned unexpected length");
        done+=static_cast<std::size_t>(n);
    }
}

FileJournal::FileJournal(const std::filesystem::path& path,std::shared_ptr<JournalIo> io)
    :owner_pid_(::getpid()),io_(std::move(io)) {
    if(!io_) throw std::invalid_argument("null journal I/O provider");
    const auto name=path.filename().string();
    if(name.empty() || name=="." || name=="..") throw std::invalid_argument("invalid journal filename");
    const auto parent=path.has_parent_path()?path.parent_path():std::filesystem::path(".");
    try {
        dir_fd_=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(dir_fd_<0) io_error("journal open directory");
        fd_=::openat(dir_fd_,name.c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
        if(fd_<0) io_error("journal open file");
        struct stat st{};
        if(::fstat(fd_,&st)!=0) io_error("journal fstat");
        if(!S_ISREG(st.st_mode)) throw std::invalid_argument("journal requires a regular file");
        while(::flock(fd_,LOCK_EX|LOCK_NB)!=0) {
            if(errno!=EINTR) io_error("journal exclusive lock");
        }
        // Re-stat under the cooperating writer lock.
        if(::fstat(fd_,&st)!=0) io_error("journal locked fstat");
        if(st.st_size<0 || static_cast<std::uint64_t>(st.st_size)>max_records*record_bytes)
            throw std::length_error("journal size limit exceeded");
        const auto size=static_cast<std::uint64_t>(st.st_size);
        const auto complete=size/record_bytes;
        for(std::uint64_t i=0;i<complete;++i) {
            Record r{}; read_exact(r.data(),r.size(),static_cast<off_t>(i*record_bytes));
            const auto event=decode(r,i+1,last_crc_);
            engine_.process(event);
            ++records_; last_crc_=static_cast<std::uint32_t>(get(r,76,4));
        }
        tail_bytes_=static_cast<std::size_t>(size%record_bytes);
        if(tail_bytes_!=0) {
            while(io_->truncate(fd_,static_cast<off_t>(complete*record_bytes))!=0) {
                if(errno!=EINTR) io_error("journal truncate incomplete tail");
            }
        }
        sync_checked(fd_);
        // The file's directory entry needs its own persistence request.
        sync_checked(dir_fd_);
        if(::lseek(fd_,0,SEEK_END)<0) io_error("journal seek append");
    } catch(...) {
        if(fd_>=0) ::close(fd_);
        if(dir_fd_>=0) ::close(dir_fd_);
        fd_=-1; dir_fd_=-1; throw;
    }
}

FileJournal::~FileJournal() {
    // Do not retry close on EINTR: Linux may already have released the fd.
    if(fd_>=0) ::close(fd_);
    if(dir_fd_>=0) ::close(dir_fd_);
}

std::uint64_t FileJournal::append(const Event& event) {
    static_assert(std::is_nothrow_move_assignable_v<EventEngine>);
    if(::getpid()!=owner_pid_) throw std::logic_error("journal handle inherited across fork");
    if(poisoned_) throw std::logic_error("journal I/O uncertain; close and recover before retry");
    if(records_>=max_records) throw std::length_error("journal record limit reached");
    // Validate/process a private candidate before any write; invalid events
    // leave both committed state and file unchanged. This favors auditability
    // over throughput; it is not on the original low-latency benchmark path.
    auto candidate=EventEngine::restore(engine_.snapshot());
    candidate.process(event);
    const auto r=encode(event,records_+1,last_crc_);
    try {
        io_->checkpoint(JournalPhase::BeforeWrite);
        write_exact(r.data(),r.size());
        io_->checkpoint(JournalPhase::AfterWrite);
        sync_checked(fd_);
        io_->checkpoint(JournalPhase::AfterSync);
        engine_=std::move(candidate);
        ++records_; last_crc_=static_cast<std::uint32_t>(get(r,76,4));
        io_->checkpoint(JournalPhase::BeforeAck);
        return records_;
    } catch(...) {
        poisoned_=true;
        throw;
    }
}

} // namespace qdev
