/*
 * archiver – sparse-file helpers (PLAN Phase 8, item 30; format v0.3).
 *
 * Shared by the serial writer path (`add_file_impl`), the I/O pipeline, and
 * the reader. A block is archived as a hole (`COMPRESSOR_SPARSE`,
 * compressed_size = 0) only when the *whole* block range is inside a hole;
 * partial holes stay normal blocks (zstd compresses runs of zeros cheaply).
 *
 * Detection is skipped entirely when fstat says the file has no holes
 * (st_blocks * 512 < st_size is false) — dense files pay one comparison,
 * and filesystems without SEEK_HOLE support never call it.
 */
#pragma once

#include "arch/format.h"
#include <crc32c/crc32c.h>
#include <xxhash.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <unordered_map>

namespace arch::io::sparse {

// True when [off, off+len) contains no allocated data. Uses SEEK_DATA: the
// next data at or past `off` (ENXIO = no data to EOF ⇒ hole) must land at or
// past off+len for the whole block to be a hole; a block starting on a data
// byte returns off itself ⇒ "not a hole". A filesystem without SEEK_HOLE/
// SEEK_DATA support (EINVAL/EOPNOTSUPP) conservatively reports "not a hole".
inline bool block_is_hole(int fd, std::uint64_t off, std::uint64_t len) {
    errno = 0;
    off_t d = ::lseek(fd, static_cast<off_t>(off), SEEK_DATA);
    if (d < 0) {
        if (errno == ENXIO) return true;  // no data past off ⇒ to EOF is hole
        return false;                     // unsupported ⇒ treat as data
    }
    return static_cast<std::uint64_t>(d) >= off + len;
}

// CRC32C over `len` zero bytes (file-checksum chain must cover hole bytes).
inline std::uint32_t crc_extend_zeros(std::uint32_t crc, std::uint64_t len) {
    static const std::uint8_t zeros[64 * 1024] = {};
    while (len) {
        const std::size_t n = static_cast<std::size_t>(
            len < sizeof zeros ? len : sizeof zeros);
        crc = crc32c::Extend(crc, zeros, n);
        len -= n;
    }
    return crc;
}

// XXH3-128 of `len` zero bytes — identical to hashing a zero-filled block
// through the normal path (dedup identity must match between a hole block
// and a dense block of zeros). Cached per length (threads compute holes of
// only a handful of distinct sizes: the block size, and one tail).
inline format::digest128 zero_digest(std::uint64_t len) {
    thread_local std::unordered_map<std::uint64_t, format::digest128> cache;
    if (auto it = cache.find(len); it != cache.end()) return it->second;

    static const std::uint8_t zeros[64 * 1024] = {};
    auto* state = XXH3_createState();
    XXH3_128bits_reset(state);
    std::uint64_t rem = len;
    while (rem) {
        const std::size_t n = static_cast<std::size_t>(
            rem < sizeof zeros ? rem : sizeof zeros);
        XXH3_128bits_update(state, zeros, n);
        rem -= n;
    }
    XXH128_hash_t h = XXH3_128bits_digest(state);
    XXH3_freeState(state);
    format::digest128 id{};
    std::memcpy(id.data(), &h.low64, 8);
    std::memcpy(id.data() + 8, &h.high64, 8);
    cache[len] = id;
    return id;
}

} // namespace arch::io::sparse
