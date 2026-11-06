#include "arch/io_uring_pool.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <iostream>
#include <cstring>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/stat.h>

namespace arch::io {

io_uring_pool::io_uring_pool(unsigned sq_depth, unsigned cq_depth, unsigned workers)
    : m_workers(workers)
{
    // NB: the third argument of io_uring_queue_init() is a FLAGS mask, not a
    // CQ depth — passing cq_depth there enabled IORING_SETUP_R_DISABLED (or
    // IOPOLL for 1) and the ring never completed anything. Use params init.
    struct io_uring_params params{};
    params.flags = 0;
    params.sq_entries = sq_depth;
    params.cq_entries = cq_depth;
    if (io_uring_queue_init_params(sq_depth, &m_ring, &params) < 0) {
        throw std::runtime_error("io_uring_queue_init_params failed");
    }

    for (unsigned i = 0; i < workers; ++i) {
        m_threads.emplace_back(&io_uring_pool::worker_loop, this, i);
    }
}

io_uring_pool::~io_uring_pool() {
    drain_and_stop();
    // io_uring_queue_quiesce() was removed in liburing 2.x; draining the CQ
    // before exit is what matters.
    struct io_uring_cqe* cqe;
    while (io_uring_peek_cqe(&m_ring, &cqe) == 0)
        io_uring_cqe_seen(&m_ring, cqe);
    io_uring_queue_exit(&m_ring);
}

int io_uring_pool::submit_read(int fd, void* buf, unsigned len,
                               uint64_t offset, void* user_data,
                               io_callback cb)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) return -EAGAIN;
    io_uring_prep_read(sqe, fd, buf, len, offset);
    uint64_t tag = next_tag();
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.emplace(tag, std::move(cb));
        m_pending.fetch_add(1, std::memory_order_relaxed); // before SQE is visible
    }

    return io_uring_submit(&m_ring);
}

int io_uring_pool::submit_write(int fd, const void* buf, unsigned len,
                                uint64_t offset, void* user_data,
                                io_callback cb)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) return -EAGAIN;
    io_uring_prep_write(sqe, fd, buf, len, offset);
    uint64_t tag = next_tag();
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.emplace(tag, std::move(cb));
        m_pending.fetch_add(1, std::memory_order_relaxed); // before SQE is visible
    }

    return io_uring_submit(&m_ring);
}

int io_uring_pool::submit_statx(int fd, const char* path, void* statxbuf,
                                unsigned flags, unsigned mask,
                                void* user_data, io_callback cb)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) return -EAGAIN;
    io_uring_prep_statx(sqe, fd, path, static_cast<int>(flags),
                        static_cast<int>(mask),
                        reinterpret_cast<struct statx*>(statxbuf));
    uint64_t tag = next_tag();
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.emplace(tag, std::move(cb));
        m_pending.fetch_add(1, std::memory_order_relaxed); // before SQE is visible
    }

    return io_uring_submit(&m_ring);
}

int io_uring_pool::submit_fsync(int fd, void* user_data, io_callback cb)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) return -EAGAIN;
    io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
    uint64_t tag = next_tag();
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.emplace(tag, std::move(cb));
        m_pending.fetch_add(1, std::memory_order_relaxed); // before SQE is visible
    }

    return io_uring_submit(&m_ring);
}

int io_uring_pool::submit_openat(int dirfd, const char* path, int flags,
                                 void* user_data, io_callback cb)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) return -EAGAIN;
    io_uring_prep_openat(sqe, dirfd, path, flags, 0);
    uint64_t tag = next_tag();
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.emplace(tag, std::move(cb));
        m_pending.fetch_add(1, std::memory_order_relaxed); // before SQE is visible
    }

    return io_uring_submit(&m_ring);
}

int io_uring_pool::submit_close(int fd, void* user_data, io_callback cb)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) return -EAGAIN;
    io_uring_prep_close(sqe, fd);
    uint64_t tag = next_tag();
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.emplace(tag, std::move(cb));
        m_pending.fetch_add(1, std::memory_order_relaxed); // before SQE is visible
    }

    return io_uring_submit(&m_ring);
}

void io_uring_pool::worker_loop(unsigned /*id*/)
{
    struct io_uring_cqe* cqe;
    // Timed wait so the loop can observe m_stopping: an untimed wait would
    // block forever on an idle ring and deadlock drain_and_stop().
    struct __kernel_timespec ts{0, 50 * 1000 * 1000}; // 50 ms

    while (!m_stopping.load(std::memory_order_acquire)) {
        uint64_t tag;
        int res;
        {
            // Serialize CQE consumption across workers (see m_cq_mtx note in
            // the header): grab the entry and acknowledge it atomically, but
            // dispatch the callback outside the lock.
            std::lock_guard cq_lk(m_cq_mtx);
            int ret = io_uring_wait_cqe_timeout(&m_ring, &cqe, &ts);
            if (ret == -EINTR || ret == -ETIME) continue;
            if (ret < 0) break;
            tag = cqe->user_data;
            res = cqe->res;
            io_uring_cqe_seen(&m_ring, cqe);
        }

        io_callback cb;
        {
            std::lock_guard lk(m_mtx);
            auto it = m_callbacks.find(tag);
            if (it != m_callbacks.end()) {
                cb = std::move(it->second);
                m_callbacks.erase(it); // each callback is consumed once
            }
        }

        if (cb) {
            cb(-1, reinterpret_cast<void*>(tag), res);
        }

        if (m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard lk(m_mtx);
            m_drain_cv.notify_all();
        }
    }
}

void io_uring_pool::drain_and_stop()
{
    // Wait for every submitted operation to be completed *and* dispatched,
    // so no callback can reference caller state after this returns.
    {
        std::unique_lock lk(m_mtx);
        m_drain_cv.wait(lk, [this] {
            return m_pending.load(std::memory_order_acquire) == 0;
        });
    }
    m_stopping.store(true, std::memory_order_release);
    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
}

} // namespace arch::io
