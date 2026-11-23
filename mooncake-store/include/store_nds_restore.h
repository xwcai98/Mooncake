#ifndef NDS_MOONCAKE_STORE_RESTORE_H
#define NDS_MOONCAKE_STORE_RESTORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mooncake_nds_read_span {
    int file_fd;
    uint64_t file_offset;
    void* destination;
    size_t bytes;
};

enum mooncake_nds_restore_result {
    MOONCAKE_NDS_RESTORE_FAILED = -1,
    MOONCAKE_NDS_RESTORE_DECLINED = 0,
    MOONCAKE_NDS_RESTORE_COMPLETE = 1
};

/*
 * Synchronous, in-process adapter. Source FDs and Store bucket read guards
 * remain valid for the call. DECLINED leaves every destination unchanged.
 * FAILED is terminal for this request. COMPLETE makes the result visible.
 */

/* Raw mode checks the owner contract. ACLFile policy selects the I/O backend. */
int mooncake_nds_restore_v2(const struct mooncake_nds_read_span* spans,
                            size_t count, int raw_mode);

/* Register caller-owned KV allocations once and release the handle before
 * freeing those allocations. Restore holds the registrations while it runs. */
int mooncake_nds_register_kv_buffers(const uintptr_t* addresses,
                                    const size_t* lengths, size_t count,
                                    void** handle);
int mooncake_nds_unregister_kv_buffers(void* handle);

/* Synchronous source vectors; their memory remains owned by the caller. */
struct mooncake_nds_write_iov {
    const void* base;
    size_t bytes;
};
int mooncake_nds_write_iov_v1(int rawfd, const struct mooncake_nds_write_iov* iov,
                              size_t count, uint64_t base, uint64_t bytes,
                              uint64_t aligned);

#ifdef __cplusplus
}
#endif
#endif
