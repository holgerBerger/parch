#include "arch/prefetch.h"
#include <algorithm>
#include <chrono>

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
    m_fd = fd;
    m_file_size = file_sz;
    m_next_offset = 0;
    m_next_delivery = 0;
    m_eof = false;
    m_chunks.clear();

    submit_prefetch_batch();
}

void prefetcher::submit_prefetch_batch()
{
    // Ensure we have enough chunks to cover the prefetch window.
    while (true) {
        // Find the highest offset we already have a chunk for.
        std::uint64_t highest = 0;
        for (auto& c : m_chunks) {
            if (c->offset + c->size > highest)
                highest = c->offset + c->size;
        }

        if (highest >= m_file_size) break;

        // How far ahead of the delivery pointer are we?
        std::uint64_t ahead = highest - m_next_delivery;
        if (ahead >= m_window_size && !m_eof) break;

        // Allocate a new chunk.
        auto* chunk = new chunk();
        chunk->offset = highest;
        chunk->size = std::min(m_chunk_size, m_file_size - highest);
        if (chunk->size == 0) {
            m_eof = true;
            m_cv.notify_all();
            delete chunk;
            break;
        }
        chunk->data.resize(chunk->size);

        auto* ptr = chunk.get(); // raw ptr for lambda capture

        int ret = m_pool.submit_read(m_fd, chunk->data.data(),
                                     static_cast<unsigned>(chunk->size),
                                     chunk->offset, ptr,
                                     [this, ptr](int, void*, int res) {
            if (res < 0) {
                // Read error — mark chunk as done with zeroed data.
            }
            ptr->ready.store(true, std::memory_order_release);
            m_cv.notify_all();
        });

        m_chunks.push_back(std::unique_ptr<chunk>(chunk)); // transfer ownership
    }
}

std::optional<std::pair<std::uint64_t, std::vector<std::uint8_t>>>
prefetcher::pop_ready_chunk()
{
    std::unique_lock lk(m_mtx);
    m_cv.wait(lk, [this] {
        if (m_eof) return true;
        // Find the next ready chunk in order.
        for (auto& c : m_chunks) {
            if (c->offset == m_next_delivery && c->ready.load(std::memory_order_acquire))
                return true;
        }
        return false;
    });

    if (m_eof && m_next_delivery >= m_file_size) {
        return std::nullopt;
    }

    // Find and extract the next chunk.
    for (auto it = m_chunks.begin(); it != m_chunks.end(); ++it) {
        if ((*it)->offset == m_next_delivery) {
            auto data = std::move((*it)->data);
            auto off  = (*it)->offset;
            m_next_delivery += (*it)->size;
            delete *it;
            m_chunks.erase(it);

            // Trigger more prefetch.
            submit_prefetch_batch();

            return std::make_pair(off, std::move(data));
        }
    }

    return std::nullopt;
}

bool prefetcher::is_done() const
{
    return m_eof && m_next_delivery >= m_file_size;
}

} // namespace arch::io
