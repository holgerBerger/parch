#include "arch/prefetch.h"
#include <algorithm>

namespace arch::io {

prefetcher::prefetcher(io_uring_pool& pool,
                       std::size_t window_size,
                       std::size_t chunk_size)
    : m_pool(pool)
    , m_window_size(window_size)
    , m_chunk_size(chunk_size)
{
}

prefetcher::~prefetcher() = default;

void prefetcher::start(int fd, std::uint64_t file_sz)
{
    std::lock_guard lk(m_mtx);
    m_fd = fd;
    m_file_size = file_sz;
    m_next_delivery = 0;
    m_eof = false;
    m_chunks.clear();

    submit_prefetch_batch();
}

void prefetcher::submit_prefetch_batch()
{
    // Ensure we have enough chunks in flight to cover the prefetch window.
    // Caller holds m_mtx.
    while (!m_eof) {
        // Find the highest offset we already have a chunk for.
        std::uint64_t highest = 0;
        for (auto& c : m_chunks) {
            if (c->offset + c->size > highest)
                highest = c->offset + c->size;
        }

        if (highest >= m_file_size) {
            // Everything has been submitted — no more chunks will appear.
            m_eof = true;
            m_cv.notify_all();
            break;
        }

        // How far ahead of the delivery pointer are we?
        if (highest - m_next_delivery >= m_window_size) break;

        // Allocate a new chunk.
        auto c = std::make_shared<chunk>();
        c->offset = highest;
        c->size = std::min(m_chunk_size, m_file_size - highest);
        c->data.resize(c->size);

        // The callback's shared_ptr copy keeps the chunk (and its kernel read
        // buffer) alive until after the ready store below; the shared_ptr is
        // destroyed by the pool worker when it discards the callback.
        m_pool.submit_read(m_fd, c->data.data(),
                           static_cast<unsigned>(c->size), c->offset, c.get(),
                           [this, c](int, void*, int res) {
            (void)res; // short read / error: undelivered bytes stay zeroed
            // Take m_mtx around store+notify: otherwise a notify can land
            // between the pop_ready_chunk predicate check and the actual
            // sleep (lost wakeup ⇒ permanent block).
            std::lock_guard lk(m_mtx);
            c->ready = true;
            m_cv.notify_all();
        });

        m_chunks.push_back(std::move(c));
    }
}

std::optional<std::pair<std::uint64_t, std::vector<std::uint8_t>>>
prefetcher::pop_ready_chunk()
{
    std::unique_lock lk(m_mtx);

    // Wait for the *next in-order* chunk to be ready, or for delivery to be
    // complete (eof reached and everything already handed out).
    m_cv.wait(lk, [this] {
        if (m_next_delivery >= m_file_size) return true; // done (also covers empty files)
        for (auto& c : m_chunks)
            if (c->offset == m_next_delivery && c->ready) return true;
        return false;
    });

    if (m_next_delivery >= m_file_size) {
        if (!m_eof) {
            // All data consumed but submissions may still be pending —
            // (re)check so eof is set for is_done().
            submit_prefetch_batch();
        }
        return std::nullopt;
    }

    // Extract the next chunk (guaranteed ready by the predicate).
    for (auto it = m_chunks.begin(); it != m_chunks.end(); ++it) {
        if ((*it)->offset == m_next_delivery && (*it)->ready) {
            auto data = std::move((*it)->data);
            auto off  = (*it)->offset;
            m_next_delivery += (*it)->size;
            m_chunks.erase(it); // callback may still hold its own reference

            submit_prefetch_batch(); // fill the window again

            return std::make_pair(off, std::move(data));
        }
    }

    return std::nullopt; // unreachable unless a read was skipped
}

bool prefetcher::is_done() const
{
    return m_eof && m_next_delivery >= m_file_size;
}

} // namespace arch::io
