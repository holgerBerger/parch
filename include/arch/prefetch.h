#pragma once

#include "arch/io_uring_pool.h"
#include "arch/format.h"
#include <vector>
#include <queue>
#include <filesystem>

namespace arch::io {

/// Prefetch manager: reads file data ahead of the compressor using io_uring.
///
/// The archiver feeds files to the prefetcher one at a time. The prefetcher
/// submits N sequential reads covering the next `window_size` bytes of data.
/// Compressed data is pulled from the ready queue via `pop_ready_chunks()`.
///
/// This is the key mechanism for bridging network-filesystem latency on small
/// files: while the compressor works on chunk K, chunks K+1 … K+N are
/// already in flight on the NVMe/FAS path.
class prefetcher {
public:
    /// @param pool        Shared io_uring pool for submissions.
    /// @param window_size Number of bytes to read-ahead (default 1 MiB).
    /// @param chunk_size  Individual read granularity (default 64 KiB).
    explicit prefetcher(io::io_uring_pool& pool,
                        std::size_t window_size = 1ULL * 1024 * 1024,
                        std::size_t chunk_size  = 64 * 1024);
    ~prefetcher();

    prefetcher(const prefetcher&) = delete;
    prefetcher& operator=(const prefetcher&) = delete;

    /// Start prefetching from a file descriptor at offset 0.
    /// @param fd      Open file descriptor.
    /// @param file_sz Total file size (precomputed so we don't over-read).
    void start(int fd, std::uint64_t file_sz);

    /// Pull the next ready chunk. Blocks if nothing is available yet and
    /// prefetch is still in progress. Returns empty optional on EOF.
    std::optional<std::pair<std::uint64_t, std::vector<std::uint8_t>>>
    pop_ready_chunk();

    /// Check if all data has been prefetched and consumed.
    bool is_done() const;

private:
    struct chunk {
        std::uint64_t offset;
        std::size_t   size;
        std::vector<std::uint8_t> data;
        std::atomic<bool> ready{false};
    };

    void submit_prefetch_batch();

    io::io_uring_pool& m_pool;
    std::size_t m_window_size;
    std::size_t m_chunk_size;

    int m_fd{-1};
    std::uint64_t m_file_size{0};
    std::uint64_t m_next_offset{0};   // where to read next
    std::uint64_t m_next_delivery{0}; // which offset to deliver next

    std::vector<std::unique_ptr<chunk>> m_chunks;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    bool m_eof{false};
};

} // namespace arch::io
