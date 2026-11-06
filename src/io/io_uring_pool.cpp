#include "arch/io_uring_pool.h"
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
    if (io_uring_queue_init(sq_depth, &m_ring, cq_depth) < 0) {
        throw std::runtime_error("io_uring_queue_init failed");
    }

    for (unsigned i = 0; i < workers; ++i) {
        m_threads.emplace_back(&io_uring_pool::worker_loop, this, i);
    }
}

io_uring_pool::~io_uring_pool() {
    drain_and_stop();
    io_uring_queue_quiesce(&m_ring, -1);
    io_uring_queue_exit(&m_ring);
}

static uint64_t next_tag(io_uring_pool& pool) {
    return pool.m_next_tag.fetch_add(1, std::memory_order_relaxed);
}

int io_uring_pool::submit_read(int fd, void* buf, unsigned len,
                               uint64_t offset, void* user_data,
                               io_callback cb)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) return -EAGAIN;
    io_uring_prep_read(sqe, fd, buf, len, offset);
    uint64_t tag = next_tag(*this);
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.push_back({tag, std::move(cb)});
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
    uint64_t tag = next_tag(*this);
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.push_back({tag, std::move(cb)});
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
    uint64_t tag = next_tag(*this);
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.push_back({tag, std::move(cb)});
    }

    return io_uring_submit(&m_ring);
}

int io_uring_pool::submit_fsync(int fd, void* user_data, io_callback cb)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) return -EAGAIN;
    io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
    uint64_t tag = next_tag(*this);
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.push_back({tag, std::move(cb)});
    }

    return io_uring_submit(&m_ring);
}

int io_uring_pool::submit_openat(int dirfd, const char* path, int flags,
                                 void* user_data, io_callback cb)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) return -EAGAIN;
    io_uring_prep_openat(sqe, dirfd, path, flags, 0);
    uint64_t tag = next_tag(*this);
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.push_back({tag, std::move(cb)});
    }

    return io_uring_submit(&m_ring);
}

int io_uring_pool::submit_close(int fd, void* user_data, io_callback cb)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) return -EAGAIN;
    io_uring_prep_close(sqe, fd);
    uint64_t tag = next_tag(*this);
    sqe->user_data = tag;

    {
        std::lock_guard lk(m_mtx);
        m_callbacks.push_back({tag, std::move(cb)});
    }

    return io_uring_submit(&m_ring);
}

void io_uring_pool::worker_loop(unsigned /*id*/)
{
    struct io_uring_cqe* cqe;
    unsigned head;

    while (!m_stopping.load(std::memory_order_acquire)) {
        int ret = io_uring_wait_cqe(&m_ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;
            break;
        }

        uint64_t tag = cqe->user_data;
        int res = cqe->res;

        io_callback cb;
        {
            std::lock_guard lk(m_mtx);
            auto it = std::find_if(m_callbacks.begin(), m_callbacks.end(),
                                   [tag](auto& p) { return p.first == tag; });
            if (it != m_callbacks.end()) {
                cb = std::move(it->second);
            }
        }

        if (cb) {
            cb(-1, reinterpret_cast<void*>(tag), res);
        }

        io_URING_CQ_REAP_MULT(&m_ring, 1, &cqe);
        io_uring_cqe_seen(&m_ring, cqe);
    }
}

void io_uring_pool::drain_and_stop()
{
    m_stopping.store(true, std::memory_order_release);
    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
}

} // namespace arch::io
