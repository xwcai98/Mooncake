#pragma once
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// A generic SDK caller cannot safely release its output on uncertain device
// completion. Strict mode requires a dedicated owner process with a real RID;
// the owner keeps its registered destination and allocation until recovery.
inline bool NdsOwnerConfigured() {
    const char* hold = std::getenv("MOONCAKE_NDS_HOLD_ON_UNKNOWN");
    const char* owner = std::getenv("MOONCAKE_NDS_OWNER_TOKEN");
    return hold && std::strcmp(hold, "1") == 0 && owner && *owner;
}

inline bool NdsSubmissionOpen() {
    if (const char* stop = std::getenv("MAC194_STOP_FILE"); stop && *stop) {
        struct stat state {};
        if (lstat(stop, &state) == 0 || errno != ENOENT) return false;
    }
    const char* text = std::getenv("MOONCAKE_NDS_DEADLINE_MONOTONIC_NS");
    if (!text) return true;
    uint64_t deadline = 0;
    const char* end = text + std::strlen(text);
    const auto parsed = std::from_chars(text, end, deadline);
    timespec now {};
    if (parsed.ec != std::errc{} || parsed.ptr != end ||
        clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return false;
    constexpr uint64_t per_second = 1000000000;
    const auto seconds = static_cast<uint64_t>(now.tv_sec);
    return seconds < deadline / per_second ||
           (seconds == deadline / per_second &&
            static_cast<uint64_t>(now.tv_nsec) < deadline % per_second);
}

[[noreturn]] inline void NdsParkUnknown(const char* operation, long detail) {
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM); sigaddset(&blocked, SIGINT); sigaddset(&blocked, SIGHUP);
    const int mask_rc = pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
    const char* owner = std::getenv("MOONCAKE_NDS_OWNER_TOKEN");
    std::fprintf(stderr, "NDS_IO_UNKNOWN operation=%s detail=%ld pid=%ld owner=%s signal_mask_rc=%d\n",
                 operation, detail, static_cast<long>(getpid()), owner ? owner : "missing", mask_rc);
    std::fflush(stderr);
    for (;;) pause();
}
