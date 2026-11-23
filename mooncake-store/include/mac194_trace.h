#pragma once

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// One process is one sample. This context is set before Store threads start.
// Only deliberately restricted identifiers enter line-oriented logs.
namespace mac194 {
inline bool safe_identifier(const char* p) {
    if (!p || !*p || std::strlen(p) > 120) return false;
    for (const unsigned char* q = reinterpret_cast<const unsigned char*>(p); *q; ++q)
        if (!(std::isalnum(*q) || *q == '-' || *q == '_' || *q == '.')) return false;
    return true;
}
inline bool safe_trace_key(const char* key) {
    if (!key || !*key || std::strlen(key) > 512) return false;
    // Connector keys include a model path and rank fields. The log protocol
    // separates fields with whitespace, so accept printable non-space ASCII.
    for (const auto* p = reinterpret_cast<const unsigned char*>(key); *p; ++p)
        if (*p < 0x21 || *p > 0x7e) return false;
    return true;
}
inline bool enabled() {
    const char* pair = std::getenv("MAC194_PAIR_ID");
    const char* arm = std::getenv("MAC194_ARM");
    const char* key = std::getenv("MAC194_KEY");
    return safe_identifier(pair) && safe_trace_key(key) && arm &&
           (!std::strcmp(arm, "A") || !std::strcmp(arm, "B"));
}
inline long long monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void event(const char* name, long long bucket, long long bytes,
                  long long status, long long elapsed_ns, long long offset = -1) {
    if (!enabled()) return;
    std::fprintf(stderr, "MAC194_EVENT pair=%s arm=%s key=%s event=%s bucket=%lld bytes=%lld status=%lld elapsed_ns=%lld offset=%lld at_ns=%lld\n",
                 std::getenv("MAC194_PAIR_ID"), std::getenv("MAC194_ARM"),
                 std::getenv("MAC194_KEY"), name, bucket, bytes, status,
                 elapsed_ns, offset, monotonic_ns());
    std::fflush(stderr);
}
inline long long get_stage_start() {
    const char* flag = std::getenv("MAC194_GET_STAGE_TIMING");
    return flag && !std::strcmp(flag, "1") ? monotonic_ns() : 0;
}
inline void get_stage_end(const char* name, long long start, long long bytes = 0) {
    if (start) event(name, -1, bytes, 0, monotonic_ns() - start);
}
inline bool scoped_key_matches(const std::string& actual) {
    if (!enabled()) return false;
    std::string expected("default\0", 8);
    expected += std::getenv("MAC194_KEY");
    return actual == expected;
}
}
