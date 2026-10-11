#pragma once

#include "qdev/engine.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sys/types.h>

namespace qdev {

enum class JournalPhase { BeforeWrite, AfterWrite, AfterSync, BeforeAck };

// Injection boundary for deterministic I/O faults. The default delegates to
// Linux syscalls. It never converts a failed write/sync into an acknowledgment.
class JournalIo {
public:
    virtual ~JournalIo() = default;
    virtual ssize_t write(int fd, const void* bytes, std::size_t count);
    virtual ssize_t read(int fd, void* bytes, std::size_t count, off_t offset);
    virtual int sync(int fd);
    virtual int truncate(int fd, off_t length);
    virtual void checkpoint(JournalPhase) {}
};

class FileJournal {
public:
    static constexpr std::size_t record_bytes = 80;
    static constexpr std::uint64_t max_records = 100000;
    // max_records * max_event_quantity bounds all accumulated quantities far
    // below signed/unsigned 64-bit limits without changing the original matcher.
    static constexpr Quantity max_event_quantity = 1000000000;
    explicit FileJournal(const std::filesystem::path& path,
                         std::shared_ptr<JournalIo> io = std::make_shared<JournalIo>());
    ~FileJournal();
    FileJournal(const FileJournal&) = delete;
    FileJournal& operator=(const FileJournal&) = delete;
    FileJournal(FileJournal&&) = delete;
    FileJournal& operator=(FileJournal&&) = delete;

    // Returns only after complete append + successful file fsync and state
    // installation. A complete unacknowledged record may recover after error.
    std::uint64_t append(const Event& event);
    [[nodiscard]] const EventEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] std::uint64_t records() const noexcept { return records_; }
    [[nodiscard]] std::size_t recovered_tail_bytes() const noexcept { return tail_bytes_; }
    [[nodiscard]] bool poisoned() const noexcept { return poisoned_; }

private:
    int fd_{-1};
    int dir_fd_{-1};
    pid_t owner_pid_{};
    std::shared_ptr<JournalIo> io_;
    EventEngine engine_;
    std::uint64_t records_{};
    std::uint32_t last_crc_{};
    std::size_t tail_bytes_{};
    bool poisoned_{false};
    void sync_checked(int fd);
    void read_exact(void* bytes, std::size_t count, off_t offset);
    void write_exact(const void* bytes, std::size_t count);
};

} // namespace qdev
