/*
 * netfs_delay — LD_PRELOAD shim that fakes network-filesystem latency
 * (PLAN Phase 9, item 32: "NFS (simulated latency)") without root.
 *
 * Environment:
 *   NETFS_PREFIX    delay only file ops on paths starting with this prefix
 *                   (the benchmark tree/archive dir — never system libs)
 *   NETFS_DELAY_US  microseconds slept per event (default 0 = disabled)
 *
 * Simulated events, like a real NFS mount:
 *   - every open()/openat()/creat() under the prefix: one RT cost
 *   - the FIRST pread()/read() on each such fd: the read RT cost
 * (subsequent reads hit "the server's socket" — deliberately not delayed so
 * large-file throughput stays bandwidth-bound and small-file latency-bound,
 * exactly the ratio the cross-file pipeline is designed to exploit.)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static long delay_us;
static const char* prefix;
static size_t prefix_len;

/* fd bookkeeping: 1 = opened under the prefix and awaiting its first-read
 * delay, 2 = read-delay already paid. Guarded by a CAS on a byte array —
 * races only ever cost/skip one simulated RT, never corrupt. */
#define MAXFD 16384
static uint8_t fd_state[MAXFD];

__attribute__((constructor)) static void nd_init(void) {
    delay_us = getenv("NETFS_DELAY_US") ? atol(getenv("NETFS_DELAY_US")) : 0;
    prefix = getenv("NETFS_PREFIX");
    prefix_len = prefix ? strlen(prefix) : 0;
    if (!delay_us || !prefix_len) delay_us = 0; // disabled
}

static int path_matched(const char* p) {
    return delay_us && p && strncmp(p, prefix, prefix_len) == 0;
}

/* True when the operation targets the simulated mount, incl. relative paths
 * (tar walks with openat(dirfd, "name", ...)): resolve dirfd/cwd through
 * /proc and re-check the prefix. */
static int target_matched(int dirfd, const char* p) {
    if (!delay_us || !p) return 0;
    if (p[0] == '/') return path_matched(p);
    char base[4096], full[8192];
    if (dirfd == AT_FDCWD) {
        if (!getcwd(base, sizeof base)) return 0;
    } else {
        char link[64];
        snprintf(link, sizeof link, "/proc/self/fd/%d", dirfd);
        ssize_t k = readlink(link, base, sizeof base - 1);
        if (k < 0) return 0;
        base[k] = '\0';
    }
    snprintf(full, sizeof full, "%s/%s", base, p);
    return strncmp(full, prefix, prefix_len) == 0;
}

static int64_t real_openat(int dirfd, const char* p, int flags, mode_t m) {
    static int64_t (*fn)(int, const char*, int, mode_t);
    if (!fn) fn = dlsym(RTLD_NEXT, "openat");
    return fn(dirfd, p, flags, m);
}

/* Absolute paths go through openat(AT_FDCWD) so tar's open() and our
 * open64/openat all funnel here. */
static int nd_openat(int dirfd, const char* p, int flags, mode_t m) {
    const int match = target_matched(dirfd, p);
    if (match) usleep(delay_us);
    int64_t fd;
    if (dirfd == AT_FDCWD && !match)
        fd = syscall(SYS_openat, dirfd, p, flags, m);
    else
        fd = real_openat(dirfd, p, flags, m);
    if (fd >= 0 && match && fd < MAXFD) fd_state[fd] = 1;
    return (int)fd;
}

int openat(int dirfd, const char* p, int flags, ...) {
    mode_t m = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
    return nd_openat(dirfd, p, flags, m);
}
int openat64(int dirfd, const char* p, int flags, ...) {
    mode_t m = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
    return nd_openat(dirfd, p, flags | O_LARGEFILE, m);
}
int open(const char* p, int flags, ...) {
    mode_t m = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
    return nd_openat(AT_FDCWD, p, flags, m);
}
int open64(const char* p, int flags, ...) {
    mode_t m = 0;
    if (flags & O_CREAT) { va_list a; va_start(a, flags); m = va_arg(a, mode_t); va_end(a); }
    return nd_openat(AT_FDCWD, p, flags | O_LARGEFILE, m);
}
FILE* fopen(const char* p, const char* mode) {
    if (path_matched(p)) usleep(delay_us);
    static FILE* (*fn)(const char*, const char*);
    if (!fn) fn = dlsym(RTLD_NEXT, "fopen");
    FILE* f = fn(p, mode);
    if (f && path_matched(p)) {
        int fd = fileno(f);
        if (fd >= 0 && fd < MAXFD) fd_state[fd] = 1;
    }
    return f;
}

static void nd_first_read_delay(int fd) {
    if (!delay_us || fd < 0 || fd >= MAXFD || fd_state[fd] != 1) return;
    fd_state[fd] = 2; // CAS-free: worst case a benign double/missed delay
    usleep(delay_us);
}

/* FORTIFY/-O2 rewrites two-arg open() calls into __open_2 (same for
 * openat → __openat_2): without these wrappers the compiler-generated calls
 * bypass the interposable `open` symbol entirely (found the hard way). */
int __open_2(const char* p, int flags) {
    return nd_openat(AT_FDCWD, p, flags, 0);
}
int __open64_2(const char* p, int flags) {
    return nd_openat(AT_FDCWD, p, flags | O_LARGEFILE, 0);
}
int __openat_2(int dirfd, const char* p, int flags) {
    return nd_openat(dirfd, p, flags, 0);
}
int __openat64_2(int dirfd, const char* p, int flags) {
    return nd_openat(dirfd, p, flags | O_LARGEFILE, 0);
}

ssize_t pread(int fd, void* b, size_t n, off_t off) {
    nd_first_read_delay(fd);
    static ssize_t (*fn)(int, void*, size_t, off_t);
    if (!fn) fn = dlsym(RTLD_NEXT, "pread");
    return fn(fd, b, n, off);
}
ssize_t pread64(int fd, void* b, size_t n, off_t off) {
    nd_first_read_delay(fd);
    static ssize_t (*fn)(int, void*, size_t, off_t);
    if (!fn) fn = dlsym(RTLD_NEXT, "pread64");
    return fn(fd, b, n, off);
}
ssize_t read(int fd, void* b, size_t n) {
    nd_first_read_delay(fd);
    static ssize_t (*fn)(int, void*, size_t);
    if (!fn) fn = dlsym(RTLD_NEXT, "read");
    return fn(fd, b, n);
}

/* close() resets the per-fd state so recycled descriptors behave. */
int close(int fd) {
    if (fd >= 0 && fd < MAXFD) fd_state[fd] = 0;
    static int (*fn)(int);
    if (!fn) fn = dlsym(RTLD_NEXT, "close");
    return fn(fd);
}
