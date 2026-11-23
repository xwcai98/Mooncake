// Restore Store spans directly into the caller's KV buffers through ACLFile batches.
#include "nds_driver.h"
#include "store_nds_restore.h"
#include "store_nds_hold.h"
#include "mac194_trace.h"

#include <acl/acl.h>
#include <aclfile.h>
#include <aclfile/ascend.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace {
constexpr size_t kBatchCapacity = 128;

struct FileRegistration {
    int fd = -1;
    aclFileHandle_t handle = nullptr;

    bool Open(int source) {
        fd = fcntl(source, F_DUPFD_CLOEXEC, 0);
        return fd >= 0 && aclFileHandleRegister(fd, &handle).code == ACLFILE_SUCCESS;
    }

    bool Close() {
        if (handle && aclFileHandleDeregister(handle).code != ACLFILE_SUCCESS)
            return false;
        handle = nullptr;
        return fd < 0 || close(std::exchange(fd, -1)) == 0;
    }

    ~FileRegistration() {
        if (!Close())
            std::fprintf(stderr, "NDS_BATCH_RESTORE_FILE_RELEASE_FAILED\n");
    }
};

struct BufferRegistration {
    void* base = nullptr;

    bool Open(void* allocation, size_t bytes) {
        const auto status = aclFileBufRegister(allocation, bytes, ACLFILE_MEMORY_NPU);
        if (status.code != ACLFILE_SUCCESS)
            return false;
        base = allocation;
        return true;
    }

    void Close() noexcept {
        if (!base)
            return;
        const auto status = aclFileBufDeregister(base);
        if (status.code != ACLFILE_SUCCESS)
            NdsParkUnknown("batch_buffer_deregister", status.code);
        base = nullptr;
    }

    ~BufferRegistration() { Close(); }
};

struct PersistentBuffer {
    BufferRegistration registration;
    size_t owners = 0;
};

struct BufferRegistry {
    std::mutex mutex;
    std::map<void*, PersistentBuffer> buffers;
};

// Explicit connector shutdown releases HAL registrations before the ACL runtime exits.
BufferRegistry& Registry() {
    static auto* registry = new BufferRegistry;
    return *registry;
}

struct KvRegistration {
    explicit KvRegistration(BufferRegistry& registry) : registry(registry) {}

    ~KvRegistration() {
        for (void* base : bases) {
            auto entry = registry.buffers.find(base);
            if (--entry->second.owners == 0)
                registry.buffers.erase(entry);
        }
    }

    BufferRegistry& registry;
    std::vector<void*> bases;
};

bool Allocation(void* destination, size_t length, void*& base, size_t& bytes) {
    if (!destination || !length ||
        aclrtMemGetAddressRange(destination, &base, &bytes) != ACL_SUCCESS || !base)
        return false;
    const auto address = reinterpret_cast<uintptr_t>(destination);
    const auto start = reinterpret_cast<uintptr_t>(base);
    return address >= start && address - start <= bytes &&
           length <= bytes - (address - start);
}

bool Plan(const mooncake_nds_read_span* spans, size_t count, int32_t device,
          const BufferRegistry& registry, std::map<int, FileRegistration>& files,
          std::map<void*, BufferRegistration>& buffers,
          std::vector<aclFileIO_t>& operations) {
    std::vector<std::pair<uintptr_t, uintptr_t>> destinations;
    destinations.reserve(count);
    operations.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& span = spans[i];
        const auto address = reinterpret_cast<uintptr_t>(span.destination);
        if (!address || !span.bytes || span.bytes > UINTPTR_MAX - address ||
            span.file_offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
            span.bytes > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - span.file_offset)
            return false;
        aclrtPtrAttributes attrs{};
        void* allocation = nullptr;
        size_t bytes = 0;
        if (aclrtPointerGetAttributes(span.destination, &attrs) != ACL_SUCCESS ||
            attrs.location.type != ACL_MEM_LOCATION_TYPE_DEVICE ||
            attrs.location.id != static_cast<uint32_t>(device) ||
            !Allocation(span.destination, span.bytes, allocation, bytes))
            return false;
        const auto start = reinterpret_cast<uintptr_t>(allocation);
        destinations.emplace_back(address, address + span.bytes);

        if (!registry.buffers.count(allocation)) {
            auto [buffer, new_buffer] = buffers.try_emplace(allocation);
            if (new_buffer && !buffer->second.Open(allocation, bytes))
                return false;
        }
        auto [file, new_file] = files.try_emplace(span.file_fd);
        if (new_file && !file->second.Open(span.file_fd))
            return false;
        operations.push_back({file->second.handle, allocation, span.bytes,
                              span.file_offset, address - start, ACLFILE_READ, nullptr});
    }
    std::sort(destinations.begin(), destinations.end());
    for (size_t i = 1; i < count; ++i) {
        if (destinations[i].first < destinations[i - 1].second)
            return false;
    }
    for (auto& operation : operations)
        operation.cookie = &operation;
    return true;
}

bool ReadBatch(aclFileBatchHandle_t batch, aclFileIO_t* operations, size_t count) {
    std::array<aclFileIOEvent_t, kBatchCapacity> events{};
    std::array<bool, kBatchCapacity> seen{};
    const auto submitted = aclFileBatchIOSubmit(batch, count, operations, 0);
    if (submitted.code != ACLFILE_SUCCESS)
        return false; // The caller destroys the batch and drains any accepted work.

    bool success = true;
    for (size_t completed = 0; completed < count;) {
        size_t received = count - completed;
        const auto status = aclFileBatchIOGetStatus(batch, 1, &received, events.data(), nullptr);
        if (status.code != ACLFILE_SUCCESS || !received || received > count - completed)
            return false;
        for (size_t i = 0; i < received; ++i) {
            const auto& event = events[i];
            const auto op = std::find_if(operations, operations + count,
                                         [&](const auto& item) { return item.cookie == event.cookie; });
            if (op == operations + count || seen[op - operations] ||
                event.status != ACLFILE_IO_COMPLETE || event.ret != op->size) {
                success = false;
                continue;
            }
            seen[op - operations] = true;
        }
        completed += received;
    }
    return success;
}

int Restore(const mooncake_nds_read_span* spans, size_t count, bool raw_mode,
            const BufferRegistry& registry) {
    if (!spans || !count)
        return MOONCAKE_NDS_RESTORE_DECLINED;
    if (raw_mode && (!NdsOwnerConfigured() || !NdsSubmissionOpen()))
        return MOONCAKE_NDS_RESTORE_FAILED;

    aclrtContext context = nullptr;
    int32_t device = -1;
    if (aclrtGetCurrentContext(&context) != ACL_SUCCESS || !context ||
        aclrtGetDevice(&device) != ACL_SUCCESS || device < 0)
        return MOONCAKE_NDS_RESTORE_DECLINED;

    if (!InitializeNdsStoreDriver())
        return MOONCAKE_NDS_RESTORE_DECLINED;

    const auto plan_start = mac194::get_stage_start();
    std::map<int, FileRegistration> files;
    std::map<void*, BufferRegistration> buffers;
    std::vector<aclFileIO_t> operations;
    if (!Plan(spans, count, device, registry, files, buffers, operations))
        return MOONCAKE_NDS_RESTORE_DECLINED;
    aclFileBatchHandle_t batch = nullptr;
    if (aclFileBatchIOSetUp(&batch, std::min(count, kBatchCapacity)).code != ACLFILE_SUCCESS)
        return MOONCAKE_NDS_RESTORE_DECLINED;
    mac194::get_stage_end("nds_batch_prepare", plan_start);

    const auto read_start = mac194::get_stage_start();
    bool success = true;
    for (size_t offset = 0; success && offset < count;) {
        const auto entries = std::min(kBatchCapacity, count - offset);
        success = (!raw_mode || NdsSubmissionOpen()) &&
                  ReadBatch(batch, operations.data() + offset, entries);
        offset += entries;
    }
    const auto destroyed = aclFileBatchIODestroy(batch);
    if (destroyed.code != ACLFILE_SUCCESS)
        NdsParkUnknown("batch_destroy", destroyed.code);
    mac194::get_stage_end("nds_batch_read_and_drain", read_start);

    const auto sync_start = mac194::get_stage_start();
    const auto synchronized = aclrtSynchronizeDevice();
    if (synchronized != ACL_SUCCESS)
        NdsParkUnknown("batch_sync", synchronized);
    mac194::get_stage_end("nds_batch_sync", sync_start);

    const auto buffer_release_start = mac194::get_stage_start();
    for (auto& entry : buffers)
        entry.second.Close();
    mac194::get_stage_end("nds_batch_buffer_release", buffer_release_start);

    const auto release_start = mac194::get_stage_start();
    for (auto& entry : files) {
        if (!entry.second.Close())
            success = false;
    }
    mac194::get_stage_end("nds_batch_file_release", release_start);
    if (std::getenv("MOONCAKE_NDS_TRACE"))
        std::fprintf(stderr, "NDS_RESTORE_BATCH spans=%zu transient_allocations=%zu success=%d\n",
                     count, buffers.size(), success);
    return success ? MOONCAKE_NDS_RESTORE_COMPLETE : MOONCAKE_NDS_RESTORE_FAILED;
}

int RestoreUnderPolicy(const mooncake_nds_read_span* spans, size_t count, bool raw_mode) {
    static bool stopped = false;
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (stopped)
        return MOONCAKE_NDS_RESTORE_FAILED;
    try {
        const auto result = Restore(spans, count, raw_mode, registry);
        stopped = result == MOONCAKE_NDS_RESTORE_FAILED ||
                  (raw_mode && result != MOONCAKE_NDS_RESTORE_COMPLETE);
        return result;
    } catch (...) {
        stopped = true;
        return MOONCAKE_NDS_RESTORE_FAILED;
    }
}
} // namespace

extern "C" int mooncake_nds_restore_v2(const mooncake_nds_read_span* spans, size_t count,
                                      int raw_mode) {
    if (raw_mode != 0 && raw_mode != 1)
        return MOONCAKE_NDS_RESTORE_FAILED;
    return RestoreUnderPolicy(spans, count, raw_mode == 1);
}

extern "C" int mooncake_nds_register_kv_buffers(const uintptr_t* addresses, const size_t* lengths,
                                                size_t count, void** handle) try {
    if (!handle)
        return ACLFILE_INVALID_ARGUMENT;
    *handle = nullptr;
    if (!addresses || !lengths || !count)
        return ACLFILE_INVALID_ARGUMENT;

    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (!InitializeNdsStoreDriver())
        return ACLFILE_NOT_SUPPORTED;

    std::map<void*, size_t> allocations;
    for (size_t i = 0; i < count; ++i) {
        void* base = nullptr;
        size_t bytes = 0;
        if (!Allocation(reinterpret_cast<void*>(addresses[i]), lengths[i], base, bytes))
            return ACLFILE_INVALID_ARGUMENT;
        allocations.try_emplace(base, bytes);
    }

    auto registration = std::make_unique<KvRegistration>(registry);
    registration->bases.reserve(allocations.size());
    for (const auto& [base, bytes] : allocations) {
        auto [entry, inserted] = registry.buffers.try_emplace(base);
        if (inserted && !entry->second.registration.Open(base, bytes)) {
            registry.buffers.erase(entry);
            return ACLFILE_NOT_SUPPORTED;
        }
        ++entry->second.owners;
        registration->bases.push_back(base);
    }
    *handle = registration.release();
    return ACLFILE_SUCCESS;
} catch (...) {
    return ACLFILE_INTERNAL_ERROR;
}

extern "C" int mooncake_nds_unregister_kv_buffers(void* handle) {
    if (!handle)
        return ACLFILE_INVALID_ARGUMENT;
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    delete static_cast<KvRegistration*>(handle);
    return ACLFILE_SUCCESS;
}
