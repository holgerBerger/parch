#pragma once

#include <linux/io_uring.h>
#include <uring.h>
#include <vector>
#include <functional>
#include <optional>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace arch::io {

/// Completion callback: invoked when a read/written SQE completes.
/// Parameters: (file_descriptor, user_data_ptr, result_bytes_or_negative_errno)
using io_callback = std::function<void(int fd, void* ud, int res)>;

/// A thread-safe io_uring submission/completion pool.
/// Manages a single ring shared among N worker threads that drain CQEs.
/// Callers submit SQEs; completions fire callbacks on whichever worker
/// thread picked up the CQE.
class io_uring_pool {
public:
    /// @param sq_depth  Submission queue depth (power of 2, min 256).
    /// @param cq_depth  Completion queue depth (power of 2, min 256).
    /// @param workers   Number of CQE-drain threads.
    explicit io_uring_pool(unsigned sq_depth = 1024,
                           unsigned cq_depth = 1024,
                           unsigned workers = 4);
    ~io_uring_pool();

    io_uring_pool(const io_uring_pool&) = delete;
    io_uring_pool& operator=(const io_uring_pool&) = delete;

    /// Submit an async read.
    /// Returns 0 on success, negative errno on failure.
    int submit_read(int fd, void* buf, unsigned len,
                    uint64_t offset, void* user_data,
                    io_callback cb);

    /// Submit an async write.
    int submit_write(int fd, const void* buf, unsigned len,
                     uint64_t offset, void* user_data,
                     io_callback cb);

    /// Submit an async statx.
    int submit_statx(int fd, const char* path, void* statxbuf,
                     unsigned flags, unsigned mask,
                     void* user_data, io_callback cb);

    /// Submit fsync for a file.
    int submit_fsync(int fd, void* user_data, io_callback cb);

    /// Submit a fixed-file-set registration (calls oneshot, not batch).
    /// Submit an openat.
    int submit_openat(int dirfd, const char* path, int flags,
                      void* user_data, io_callback cb);

    /// Submit close.
    int submit_close(int fd, void* user_data, io_callback cb);

    /// Wait until all pending submissions complete, then destroy workers.
    void drain_and_stop();

private:
    void worker_loop(unsigned id);

    ::io_uring m_ring;
    unsigned   m_workers;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_stopping{false};

    // Callback registry — protected by m_mtx during submission,
    // read-only during CQE drain (each callback is consumed once).
    std::mutex m_mtx;
    // Map user_data pointer → callback (cleared on completion).
    std::vector<std::pair<uint64_t, io_callback>> m_callbacks;
    std::atomic<uint64_t> m_next_tag{0}; // unique tag per submission
};

} // namespace arch::io
