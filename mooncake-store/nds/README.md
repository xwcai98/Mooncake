# Mooncake Store NDS adapter

Build Mooncake Store with `-DUSE_NDS_STORE=ON`. Build NDS as shared libraries
with Ascend support before building the adapter.

```sh
cmake -S mooncake-store/nds -B build-nds-adapter \
  -DNDS_SOURCE=/path/to/NDS-A5 \
  -DNDS_BUILD=/path/to/nds-build \
  -DCANN_ROOT=/path/to/cann/aarch64-linux
cmake --build build-nds-adapter
```

Set `MOONCAKE_STORE_NDS_LIBRARY` to the built
`libnds_mooncake_primary.so` and make the NDS and CANN runtime libraries
available to the loader. The default file bucket remains available. For
`MOONCAKE_NDS_WRITE_MODE=raw-primary`, configure an exclusive unmounted local
NVMe partition for the raw store. Set `MOONCAKE_NDS_RAW_KIND=partition`,
`MOONCAKE_NDS_RAW_DEVICE`, `MOONCAKE_NDS_RAW_OFFSET` and
`MOONCAKE_NDS_RAW_BYTES`, and provide the expected device identity with
`MOONCAKE_NDS_RAW_BDF`, `MOONCAKE_NDS_RAW_NSID`,
`MOONCAKE_NDS_RAW_SERIAL`, `MOONCAKE_NDS_RAW_PARTITION`,
`MOONCAKE_NDS_RAW_PART_START` and `MOONCAKE_NDS_RAW_PART_SECTORS`. Partition
start and size use sysfs 512-byte sectors. The configured raw interval uses
partition-relative bytes. Raw restores require the existing owner process
contract through `MOONCAKE_NDS_HOLD_ON_UNKNOWN=1` and
`MOONCAKE_NDS_OWNER_TOKEN`. The raw bucket index is in memory and is lost
when the Store process exits.

For the Qwen layout, use one object per raw bucket and set
`MOONCAKE_NDS_ALIGNED_PAYLOAD=1` for both offload and restore. Apply
`vllm-ascend-kv-lifecycle.patch` in the matching vLLM-Ascend source tree
to register each HBM allocation at KV cache setup and deregister it after
the Store worker has drained. The adapter reads directly into the final
KV ranges with `aclFileBatchIOSubmit` and per-range buffer offsets.
ACLFile policy selects UB direct or staging per request. Use its
`require_direct` policy when measuring direct I/O and confirm the actual
backend with UB ioctl traces.
