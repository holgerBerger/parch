/*
 * archiver – transient-error retry with backoff (PLAN Phase 8, item 31).
 *
 * Network filesystems (NFS/SMB) surface transient failures as EIO / EAGAIN /
 * ESTALE / ETIMEDOUT on otherwise-successful syscalls; a bounded retry with
 * exponential backoff keeps one blip from failing a whole archive run.
 * EINTR is retried immediately (not counted, no backoff).
 */
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <ctime>

namespace arch::detail {

inline bool is_transient_errno(int e) {
    switch (e) {
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK: // distinct value on non-Linux platforms
#endif
        case EINTR:
        case EIO:
        case ESTALE:
        case ETIMEDOUT:
        case ENETDOWN:
        case ENETUNREACH:
        case EHOSTDOWN:
        case EBUSY:
            return true;
        default:
            return false;
    }
}

// Test hook: total backoff sleeps performed process-wide (unit tests watch
// it to prove the retry path actually ran without real network latency).
inline std::atomic<std::uint64_t>& retry_sleep_count() {
    static std::atomic<std::uint64_t> n{0};
    return n;
}

// Sleep min(1ms << attempt, 100ms). nanosleep, EINTR-restarted.
inline void backoff_sleep(int attempt) {
    retry_sleep_count().fetch_add(1, std::memory_order_relaxed);
    long ns = 1000000L << (attempt < 7 ? attempt : 7); // 1,2,4,...,100ms cap
    if (ns > 100000000L) ns = 100000000L;
    const struct timespec req{ns / 1000000000L, ns % 1000000000L};
    struct timespec rem;
    int r;
    do { r = ::nanosleep(&req, &rem); } while (r != 0 && errno == EINTR);
}

// Run `op()` (pread/pwrite-style: returns bytes or -1 + errno). Transient
// failures are retried up to `max_attempts` times with exponential backoff;
// EINTR never consumes an attempt. Partial progress (`0 <= r < want`) is
// returned to the caller to re-loop with its own offset bookkeeping.
template <typename Op>
ssize_t retry_io(Op op, int max_attempts = 5) {
    int attempt = 0;
    for (;;) {
        errno = 0;
        const ssize_t r = op();
        if (r >= 0) return r;
        const int e = errno;
        if (e == EINTR) continue; // free retry, not counted
        if (is_transient_errno(e) && attempt < max_attempts) {
            backoff_sleep(attempt++);
            continue;
        }
        errno = e;
        return r;
    }
}

} // namespace arch::detail
