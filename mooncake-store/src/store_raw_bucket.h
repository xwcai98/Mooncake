// Own the claimed raw partition and the in-memory index of synced buckets.
#pragma once

#include "store_nds_hold.h"
#include "store_nds_restore.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mooncake::nds_store {

class RawBucketStore {
   public:
    static constexpr uint64_t kAlignment = 4096;
    struct Source {
        int fd;
        uint64_t base;
        uint64_t bytes;
    };

    RawBucketStore(const RawBucketStore&) = delete;
    RawBucketStore& operator=(const RawBucketStore&) = delete;
    ~RawBucketStore() {
        if (fd_ >= 0) close(fd_);
        if (adapter_) dlclose(adapter_);
    }

    static std::unique_ptr<RawBucketStore> Open() {
        const char* path = std::getenv("MOONCAKE_NDS_RAW_DEVICE");
        const char* kind = std::getenv("MOONCAKE_NDS_RAW_KIND");
        const char* library = std::getenv("MOONCAKE_STORE_NDS_LIBRARY");
        const char* bdf = std::getenv("MOONCAKE_NDS_RAW_BDF");
        const char* serial = std::getenv("MOONCAKE_NDS_RAW_SERIAL");
        uint64_t offset, bytes, nsid, partition, start, sectors;
        if (!path || !kind || std::string_view(kind) != "partition" ||
            !library || !bdf || !*bdf || !serial || !*serial ||
            !Parse(std::getenv("MOONCAKE_NDS_RAW_OFFSET"), offset) ||
            !Parse(std::getenv("MOONCAKE_NDS_RAW_BYTES"), bytes) ||
            !Parse(std::getenv("MOONCAKE_NDS_RAW_NSID"), nsid) || !nsid ||
            !Parse(std::getenv("MOONCAKE_NDS_RAW_PARTITION"), partition) || !partition ||
            !Parse(std::getenv("MOONCAKE_NDS_RAW_PART_START"), start) ||
            !Parse(std::getenv("MOONCAKE_NDS_RAW_PART_SECTORS"), sectors) || !sectors ||
            sectors > UINT64_MAX / 512 || !bytes || offset % kAlignment || bytes % kAlignment ||
            offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
            bytes > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - offset) {
            return nullptr;
        }

        auto store = std::unique_ptr<RawBucketStore>(new RawBucketStore(offset, bytes));
        store->adapter_ = dlopen(library, RTLD_NOW | RTLD_LOCAL);
        if (!store->adapter_) return nullptr;
        store->write_ = reinterpret_cast<decltype(store->write_)>(
            dlsym(store->adapter_, "mooncake_nds_write_iov_v1"));
        store->restore_ = reinterpret_cast<decltype(store->restore_)>(
            dlsym(store->adapter_, "mooncake_nds_restore_v2"));
        if (!store->write_ || !store->restore_) return nullptr;

        store->fd_ = open(path, O_RDWR | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_DSYNC);
        if (store->fd_ < 0) return nullptr;
        struct stat info {};
        uint64_t capacity = 0;
        int sector_bytes = 0;
        if (fstat(store->fd_, &info) || !S_ISBLK(info.st_mode) ||
            ioctl(store->fd_, BLKGETSIZE64, &capacity) ||
            ioctl(store->fd_, BLKSSZGET, &sector_bytes) || sector_bytes != 512 ||
            capacity != sectors * 512 || offset > capacity || bytes > capacity - offset ||
            !CheckPartition(info.st_rdev, bdf, nsid, serial, partition, start, sectors)) {
            return nullptr;
        }
        return store;
    }

    bool Append(int64_t bucket, const std::vector<iovec>& iovs, uint64_t bytes) {
        std::lock_guard lock(mutex_);
        if (stopped_ || !bytes || bytes > UINT64_MAX - (kAlignment - 1) ||
            buckets_.count(bucket) || !NdsSubmissionOpen())
            return false;

        const auto length = (bytes + kAlignment - 1) & ~(kAlignment - 1);
        if (length > end_ - next_) return false;
        std::vector<mooncake_nds_write_iov> input;
        input.reserve(iovs.size());
        for (const auto& iov : iovs)
            input.push_back({iov.iov_base, iov.iov_len});

        const auto base = next_;
        next_ += length;  // Failed writes can leave data on the device.
        if (write_(fd_, input.data(), input.size(), base, bytes, length) !=
            MOONCAKE_NDS_RESTORE_COMPLETE) {
            stopped_ = true;
            return false;
        }
        int result;
        do { result = fdatasync(fd_); } while (result && errno == EINTR);
        if (result) {
            stopped_ = true;
            return false;
        }
        buckets_.emplace(bucket, Source{fd_, base, bytes});
        return true;
    }

    bool Ready() {
        std::lock_guard lock(mutex_);
        return !stopped_;
    }

    std::optional<Source> Find(int64_t bucket) {
        std::lock_guard lock(mutex_);
        const auto entry = buckets_.find(bucket);
        if (entry == buckets_.end()) return std::nullopt;
        return entry->second;
    }

    bool Restore(const std::vector<mooncake_nds_read_span>& spans) {
        std::lock_guard lock(mutex_);
        if (stopped_ || !NdsSubmissionOpen()) return false;
        if (restore_(spans.data(), spans.size(), 1) == MOONCAKE_NDS_RESTORE_COMPLETE)
            return true;
        stopped_ = true;
        return false;
    }

   private:
    RawBucketStore(uint64_t begin, uint64_t length) : next_(begin), end_(begin + length) {}

    static bool Parse(const char* input, uint64_t& result) {
        if (!input || !*input) return false;
        const std::string_view text(input);
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, result);
        return parsed.ec == std::errc{} && parsed.ptr == end;
    }

    static bool CheckPartitionNode(const std::filesystem::path& node, const char* bdf,
                                   uint64_t nsid, const char* serial, uint64_t part,
                                   uint64_t start, uint64_t size) {
        const auto parent = node.parent_path();
        uint64_t actual_part = 0, actual_start = 0, actual_size = 0, actual_nsid = 0;
        std::string actual_serial, transport;
        std::ifstream partition_file(node / "partition"), start_file(node / "start"),
            size_file(node / "size"), nsid_file(parent / "nsid"),
            serial_file(parent / "device" / "serial"),
            transport_file(parent / "device" / "transport");
        if (!(partition_file >> actual_part) || !(start_file >> actual_start) ||
            !(size_file >> actual_size) || !(nsid_file >> actual_nsid) ||
            !(serial_file >> actual_serial) || !(transport_file >> transport) ||
            actual_part != part || actual_start != start || actual_size != size ||
            actual_nsid != nsid || actual_serial != serial || transport != "pcie") return false;
        for (auto p = parent; p.has_parent_path() && p != p.parent_path(); p = p.parent_path())
            if (p.filename() == bdf) return true;
        return false;
    }

    static bool CheckPartition(dev_t dev, const char* bdf, uint64_t nsid,
                               const char* serial, uint64_t part,
                               uint64_t start, uint64_t size) {
        const auto link = std::filesystem::path("/sys/dev/block") /
                          (std::to_string(major(dev)) + ":" + std::to_string(minor(dev)));
        std::error_code error;
        const auto node = std::filesystem::canonical(link, error);
        return !error && CheckPartitionNode(node, bdf, nsid, serial, part, start, size);
    }

    void* adapter_ = nullptr;
    decltype(&mooncake_nds_restore_v2) restore_ = nullptr;
    decltype(&mooncake_nds_write_iov_v1) write_ = nullptr;
    int fd_ = -1;
    uint64_t next_;
    const uint64_t end_;
    std::mutex mutex_;
    bool stopped_ = false;
    std::map<int64_t, Source> buckets_;
};

}  // namespace mooncake::nds_store
