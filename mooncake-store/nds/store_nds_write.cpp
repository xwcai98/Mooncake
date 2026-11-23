#include "nds_driver.h"
#include "store_nds_restore.h"
#include "store_nds_hold.h"
#include "mac194_trace.h"
#include <acl/acl.h>
#include <aclfile.h>
#include <aclfile/ascend.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <vector>

namespace {
constexpr uint64_t kPage = 4096, kChunk = 16 * 1024 * 1024;
using Clock = std::chrono::steady_clock;
uint64_t elapsed(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count();
}
struct Lease {
    int fd = -1;
    aclFileHandle_t file = nullptr;
    void* hbm = nullptr;
    std::vector<char>* host = nullptr;
    bool registered = false, retain = false;
    bool Release() {
        if (retain) return false;
        if (file && aclFileHandleDeregister(file).code != ACLFILE_SUCCESS) {
            std::fprintf(stderr, "NDS_WRITE_RETAIN step=deregister_file fd=%d hbm=%p\n", fd, hbm);
            retain = true; return false;
        }
        file = nullptr;
        if (registered && aclFileBufDeregister(hbm).code != ACLFILE_SUCCESS) {
            std::fprintf(stderr, "NDS_WRITE_RETAIN step=deregister_hbm fd=%d hbm=%p\n", fd, hbm);
            retain = true; return false;
        }
        registered = false;
        if (hbm && aclrtFree(hbm) != ACL_SUCCESS) {
            std::fprintf(stderr, "NDS_WRITE_RETAIN step=free_hbm fd=%d hbm=%p\n", fd, hbm);
            retain = true; return false;
        }
        hbm = nullptr;
        if (fd >= 0) { int old = fd; fd = -1; if (close(old)) return false; }
        delete host; host = nullptr;
        return true;
    }
    ~Lease() { if (!retain) (void)Release(); }
};
bool DeviceId(int32_t& out) {
    const char* p = std::getenv("MOONCAKE_NDS_DEVICE_ID");
    if (!p || !*p) return false;
    std::string_view s(p); uint32_t value = 0;
    auto parsed = std::from_chars(s.data(), s.data() + s.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() ||
        value > static_cast<uint32_t>(INT32_MAX)) return false;
    out = static_cast<int32_t>(value); return true;
}
bool ValidateIovs(const mooncake_nds_write_iov* iov, size_t count, uint64_t bytes) {
    if (!iov || !count) return false;
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if ((iov[i].bytes && !iov[i].base) || iov[i].bytes > bytes - total) return false;
        total += iov[i].bytes;
    }
    return total == bytes;
}
bool CopyIovs(const mooncake_nds_write_iov* iov, size_t count, char* dst,
              size_t bytes, uint64_t offset) {
    for (size_t i = 0; i < count && bytes; ++i) {
        if (offset >= iov[i].bytes) { offset -= iov[i].bytes; continue; }
        const size_t available = iov[i].bytes - static_cast<size_t>(offset);
        const size_t take = std::min(available, bytes);
        std::memcpy(dst, static_cast<const char*>(iov[i].base) + offset, take);
        dst += take; bytes -= take; offset = 0;
    }
    return bytes == 0;
}
int Fail(Lease& lease, bool& stopped, const char* step, long detail) {
    lease.retain = true; stopped = true;
    std::fprintf(stderr, "NDS_IO_TERMINAL step=%s detail=%ld retained_fd=%d hbm=%p host=%p\n",
                 step, detail, lease.fd, lease.hbm,
                 lease.host ? static_cast<void*>(lease.host->data()) : nullptr);
    return MOONCAKE_NDS_RESTORE_FAILED;
}
[[noreturn]] void Unknown(Lease& lease, bool& stopped, const char* step, long detail) {
    lease.retain = true; stopped = true;
    std::fprintf(stderr, "NDS_WRITE_RETAIN fd=%d hbm=%p host=%p\n", lease.fd, lease.hbm,
                 lease.host ? static_cast<void*>(lease.host->data()) : nullptr);
    NdsParkUnknown(step, detail);
}
int Reject(const char* step, long detail) {
    std::fprintf(stderr, "NDS_WRITE_REJECT step=%s detail=%ld\n", step, detail);
    return MOONCAKE_NDS_RESTORE_FAILED;
}
int Write(int rawfd, const mooncake_nds_write_iov* iov,
          size_t iov_count, uint64_t base, uint64_t bytes, uint64_t aligned,
          bool& stopped) {
    if (!NdsSubmissionOpen()) return Reject("submission_closed", 0);
    struct stat dst{};
    uint64_t capacity = 0;
    if (!bytes || base % kPage || aligned % kPage || aligned < bytes ||
        bytes > UINT64_MAX - (kPage - 1) ||
        aligned != ((bytes + kPage - 1) & ~(kPage - 1)) ||
        !ValidateIovs(iov, iov_count, bytes) || fstat(rawfd, &dst))
        return Reject("input_or_stat", errno);
    if (!S_ISBLK(dst.st_mode) || ioctl(rawfd, BLKGETSIZE64, &capacity))
        return Reject("block_capacity", errno);
    if (
        base > capacity || aligned > capacity - base ||
        base > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        aligned > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - base)
        return Reject("extent", 0);
    int32_t device = -1;
    if (!NdsOwnerConfigured()) return Reject("owner_contract", 0);
    if (!DeviceId(device)) return Reject("device_id", 0);
    const auto bind_rc = aclrtSetDevice(device);
    if (bind_rc != ACL_SUCCESS) return Reject("set_device", bind_rc);
    aclrtContext ctx = nullptr;
    const auto context_rc = aclrtGetCurrentContext(&ctx);
    if (context_rc != ACL_SUCCESS || !ctx) return Reject("current_context", context_rc);
    if (!InitializeNdsStoreDriver()) return Reject("aclfile_enable", 0);
    Lease lease; lease.fd = fcntl(rawfd, F_DUPFD_CLOEXEC, 0);
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(aligned, kChunk));
    if (lease.fd < 0) return Reject("duplicate_fd", errno);
    auto status = aclFileHandleRegister(lease.fd, &lease.file);
    if (status.code != ACLFILE_SUCCESS) return Reject("register_file", status.code);
    auto alloc_rc = aclrtMalloc(&lease.hbm, chunk, ACL_MEM_MALLOC_HUGE_FIRST);
    if (alloc_rc != ACL_SUCCESS) return Reject("malloc_hbm", alloc_rc);
    status = aclFileBufRegister(lease.hbm, chunk, ACLFILE_MEMORY_NPU);
    if (status.code != ACLFILE_SUCCESS) return Reject("register_hbm", status.code);
    lease.registered = true;
    lease.host = new std::vector<char>(chunk);
    // From this point any exception or uncertain device completion retains all
    // transfer resources, including the H2D source, until owner recovery.
    lease.retain = true;
    auto& host = *lease.host;
    uint64_t read_ns = 0, h2d_ns = 0, write_ns = 0;
    auto stop_write = [&](uint64_t completed) {
        stopped = true;
        lease.retain = false;
        const bool closed = lease.Release();
        std::fprintf(stderr, "NDS_SUBMIT_STOP operation=write completed=%llu closed=%d\n",
                     static_cast<unsigned long long>(completed), closed);
        return MOONCAKE_NDS_RESTORE_FAILED;
    };
    for (uint64_t pos = 0; pos < aligned;) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(chunk, aligned - pos));
        const size_t payload = static_cast<size_t>(std::min<uint64_t>(count, bytes - std::min(bytes, pos)));
        std::fill(host.begin(), host.begin() + count, 0);
        auto t = Clock::now();
        if (!CopyIovs(iov, iov_count, host.data(), payload, pos))
            return Fail(lease, stopped, "copy_iov", 0);
        read_ns += elapsed(t); t = Clock::now();
        if (!NdsSubmissionOpen()) return stop_write(pos);
        auto rc = aclrtMemcpy(lease.hbm, chunk, host.data(), count, ACL_MEMCPY_HOST_TO_DEVICE);
        if (rc != ACL_SUCCESS) Unknown(lease, stopped, "h2d", rc);
        rc = aclrtSynchronizeDevice();
        if (rc != ACL_SUCCESS) Unknown(lease, stopped, "h2d_sync", rc);
        h2d_ns += elapsed(t); t = Clock::now();
        if (!NdsSubmissionOpen()) return stop_write(pos);
        mac194::event("ub_write_submit", -1, count, 0, 0, base + pos);
        const auto submit_start = Clock::now();
        if (!NdsSubmissionOpen()) {
            mac194::event("ub_write_cancel", -1, 0, 0, 0, base + pos);
            return stop_write(pos);
        }
        const auto wrote = aclFileWrite(lease.file, lease.hbm, count, static_cast<off_t>(base + pos), 0);
        mac194::event("ub_write_return", -1, wrote, wrote == static_cast<ssize_t>(count) ? 0 : -1, elapsed(submit_start), base + pos);
        if (wrote == -ACLFILE_NOT_SUPPORTED || wrote == -ACLFILE_INVALID_ARGUMENT)
            return Fail(lease, stopped, "direct_rejected", wrote);
        if (wrote < 0) Unknown(lease, stopped, "direct_write", wrote);
        if (wrote != static_cast<ssize_t>(count)) {
            rc = aclrtSynchronizeDevice();
            if (rc != ACL_SUCCESS) Unknown(lease, stopped, "short_write_sync", rc);
            return Fail(lease, stopped, "short_write", wrote);
        }
        rc = aclrtSynchronizeDevice();
        if (rc != ACL_SUCCESS) Unknown(lease, stopped, "write_sync", rc);
        write_ns += elapsed(t); pos += count;
    }
    lease.retain = false;
    if (!lease.Release()) {
        stopped = true;
        std::fprintf(stderr, "NDS_IO_TERMINAL step=release detail=-1\n");
        return MOONCAKE_NDS_RESTORE_FAILED;
    }
    std::fprintf(stderr, "NDS_WRITE_COMPLETE mode=require_direct base=%llu bytes=%llu aligned=%llu read_ns=%llu h2d_ns=%llu write_ns=%llu\n",
                 (unsigned long long)base, (unsigned long long)bytes, (unsigned long long)aligned,
                 (unsigned long long)read_ns, (unsigned long long)h2d_ns, (unsigned long long)write_ns);
    return MOONCAKE_NDS_RESTORE_COMPLETE;
}
int WriteLocked(int rawfd, const mooncake_nds_write_iov* iov,
                size_t iov_count, uint64_t base, uint64_t bytes, uint64_t aligned) {
    static std::mutex mutex; static bool stopped = false;
    std::lock_guard<std::mutex> lock(mutex);
    try {
        return stopped ? MOONCAKE_NDS_RESTORE_FAILED :
            Write(rawfd, iov, iov_count, base, bytes, aligned, stopped);
    } catch (...) {
        stopped = true;
        NdsParkUnknown("writer_exception", -1);
    }
}
} // namespace

extern "C" int mooncake_nds_write_iov_v1(int rawfd, const mooncake_nds_write_iov* iov,
                                           size_t count, uint64_t base,
                                           uint64_t bytes, uint64_t aligned) {
    if (!iov) return Reject("iov_null", 0);
    return WriteLocked(rawfd, iov, count, base, bytes, aligned);
}
