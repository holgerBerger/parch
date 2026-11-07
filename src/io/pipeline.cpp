#include "arch/pipeline.h"
#include "arch/compressor.h"
#include "io/sparse.h"
#include "util/retry.h"
#include "util/xattr.h"
#include <spdlog/spdlog.h>
#include <xxhash.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <span>
#include <crc32c/crc32c.h>

namespace arch::io {

namespace {

format::digest128 xxh3_128(std::span<const std::uint8_t> d) {
    XXH128_hash_t h = XXH3_128bits(d.data(), d.size());
    format::digest128 id{};
    std::memcpy(id.data(), &h.low64, 8);
    std::memcpy(id.data() + 8, &h.high64, 8);
    return id;
}

} // namespace

pipeline::pipeline(options opts, std::string compressor, int level,
                   bool use_compression, bool dedup)
    : m_opts(opts), m_compressor(std::move(compressor)), m_level(level),
      m_use_compression(use_compression), m_dedup(dedup)
{
    if (m_opts.files_in_flight < 1) m_opts.files_in_flight = 1;
    if (m_opts.compress_threads < 1) m_opts.compress_threads = 1;
    if (m_opts.per_file_cap < 1) m_opts.per_file_cap = 1;
    // Liveness: with every in-flight file at its cap, the oldest file's next
    // acquisition must still fit: files_in_flight * per_file_cap < block_permits
    // (slack of one permit). Scale the pool up rather than break the bound.
    const int need = m_opts.files_in_flight * m_opts.per_file_cap + 1;
    if (m_opts.block_permits < need) m_opts.block_permits = need;

    for (int i = 0; i < m_opts.files_in_flight; ++i)
        m_io_threads.emplace_back(&pipeline::io_worker, this);
    for (int i = 0; i < m_opts.compress_threads; ++i)
        m_comp_threads.emplace_back(&pipeline::compress_worker, this);
}

pipeline::~pipeline()
{
    {
        std::lock_guard lk(m_mtx);
        m_stopping.store(true, std::memory_order_release);
    }
    m_cv.notify_all();
    for (auto& t : m_io_threads) t.join();
    for (auto& t : m_comp_threads) t.join();
}

std::uint64_t pipeline::submit(std::filesystem::path path, std::uint64_t file_size,
                               bool follow_symlinks)
{
    auto fs_rec = std::make_shared<file_state>();
    fs_rec->path = std::move(path);
    fs_rec->nblocks = static_cast<std::size_t>(
        (file_size + format::ARCH_BLOCK_SIZE - 1) / format::ARCH_BLOCK_SIZE);
    fs_rec->slots.resize(fs_rec->nblocks);
    fs_rec->follow_symlinks = follow_symlinks;

    std::unique_lock lk(m_mtx);
    // Backpressure: the writer must keep draining the head while we wait.
    m_cv.wait(lk, [this] {
        return m_stopping.load(std::memory_order_acquire) ||
               m_files.size() < static_cast<std::size_t>(m_opts.files_in_flight);
    });
    std::uint64_t seq = m_next_seq++;
    m_files.emplace(seq, fs_rec);
    m_io_queue.emplace_back(seq, std::move(fs_rec));
    m_cv.notify_all();
    return seq;
}

bool pipeline::next_block(std::uint64_t seq, std::size_t index, pipeline_block& out)
{
    std::unique_lock lk(m_mtx);
    auto it = m_files.find(seq);
    if (it == m_files.end()) return false;
    auto& f = *it->second;
    // Block `index` is deliverable once its slot is ready. The wait ends
    // early on a file error or shutdown; when I/O is done and every produced
    // block has left the compression stage, a still-missing slot means the
    // block never existed (file shrank between submit and fstat).
    m_cv.wait(lk, [&] {
        if (m_stopping.load(std::memory_order_acquire)) return true;
        if (f.error) return true;
        if (index < f.slots.size() && f.slots[index].ready) return true;
        return f.io_done && f.queued_raw == 0 && f.in_compress == 0;
    });
    if (m_stopping.load(std::memory_order_acquire)) return false;
    if (f.error) return false;                      // reported in wait_file_done
    if (index >= f.slots.size()) return false;      // past the produced blocks
    auto& slot = f.slots[index];
    if (!slot.ready) return false;                  // never produced (short file)
    out = std::move(slot.blk);
    slot.ready = false;
    return true;
}

void pipeline::release_block(std::uint64_t seq)
{
    std::lock_guard lk(m_mtx);
    auto it = m_files.find(seq);
    if (it == m_files.end()) return;  // already reclaimed by wait_file_done
    auto& f = *it->second;
    if (f.permits_held > 0 && m_permits_used > 0) {
        f.permits_held--;
        m_permits_used--;
    }
    m_cv.notify_all();
}

pipeline_file pipeline::wait_file_done(std::uint64_t seq)
{
    std::unique_lock lk(m_mtx);
    auto it = m_files.find(seq);
    if (it == m_files.end()) return {};
    auto f = it->second;
    m_cv.wait(lk, [&] {
        return f->io_done || m_stopping.load(std::memory_order_acquire);
    });

    pipeline_file out;
    out.path = f->path;
    out.error = f->error;
    out.st = f->st;
    out.file_crc32c = f->file_crc;
    out.xattr_blob = std::move(f->xattr_blob);
    out.acl_blob = std::move(f->acl_blob);

    // Reclaim permits for blocks that live in this file's slots (produced but
    // never consumed/released). Blocks still queued raw or in a compression
    // worker's hands are released by that worker itself — including after this
    // erase, because the worker re-looks-up `seq` under the store lock.
    const int stranded = std::max(0, f->permits_held - f->queued_raw - f->in_compress);
    f->permits_held -= stranded;
    m_permits_used = std::max(0, m_permits_used - stranded);

    m_files.erase(it); // frees an in-flight slot
    m_cv.notify_all();
    return out;
}

std::size_t pipeline::in_flight()
{
    std::lock_guard lk(m_mtx);
    return m_files.size();
}

// ---- I/O worker: one file at a time, sequential preads. Parallelism and the
// network-latency win come from many files being open at once, not from
// within-file readahead (per-file order is the natural delivery order). ----
void pipeline::io_worker()
{
    for (;;) {
        std::uint64_t seq;
        std::shared_ptr<file_state> f;
        {
            std::unique_lock lk(m_mtx);
            m_cv.wait(lk, [this] {
                return m_stopping.load(std::memory_order_acquire) ||
                       !m_io_queue.empty();
            });
            if (m_stopping.load(std::memory_order_acquire)) return;
            auto front = m_io_queue.front();
            m_io_queue.pop_front();
            seq = front.first;
            f = front.second;
        }

        int fd = static_cast<int>(arch::detail::retry_io(
            [&] { return ::open(f->path.c_str(),
                                O_RDONLY | (f->follow_symlinks ? 0 : O_NOFOLLOW)); }));
        if (fd < 0) {
            int e = errno;
            std::lock_guard lk(m_mtx);
            f->error = std::make_error_code(static_cast<std::errc>(e));
            f->io_done = true;
            m_cv.notify_all();
            continue;
        }

        struct stat st{};
        if (arch::detail::retry_io([&] { return ::fstat(fd, &st); }) != 0) {
            int e = errno;
            ::close(fd);
            std::lock_guard lk(m_mtx);
            f->error = std::make_error_code(static_cast<std::errc>(e));
            f->io_done = true;
            m_cv.notify_all();
            continue;
        }

        // Metadata reads happen here too — on a network FS these round-trips
        // are part of what cross-file concurrency hides.
        try {
            util::read_xattrs(f->path, f->xattr_blob);
            util::read_acl(f->path, f->acl_blob);
        } catch (const std::exception& ex) {
            SPDLOG_DEBUG("xattr read failed for {}: {}", f->path.c_str(), ex.what());
        }

        {
            std::lock_guard lk(m_mtx);
            f->st = st;
            // The caller's size was an estimate (walk-time stat); trust the
            // fstat. Re-size the slot vector if the file changed underneath.
            const std::size_t nb = static_cast<std::size_t>(
                (static_cast<std::uint64_t>(st.st_size) + format::ARCH_BLOCK_SIZE - 1) /
                format::ARCH_BLOCK_SIZE);
            f->nblocks = nb;
            f->slots.resize(nb);
        }

        std::uint64_t off = 0;
        const std::uint64_t size = static_cast<std::uint64_t>(st.st_size);
        // Hole scanning only when fstat already proves the file is sparse
        // (allocated size < apparent size); dense files pay nothing.
        const bool sparse_file =
            m_opts.sparse && size > 0 &&
            static_cast<std::uint64_t>(st.st_blocks) * 512 < size;
        std::uint32_t fcrc = 0;
        std::size_t idx = 0;
        std::error_code err;
        while (off < size) {
            const std::size_t bsize = static_cast<std::size_t>(
                std::min<std::uint64_t>(format::ARCH_BLOCK_SIZE, size - off));

            // One permit per live block; the per-file cap keeps the head of
            // the queue from starving behind the other in-flight files.
            {
                std::unique_lock lk(m_mtx);
                m_cv.wait(lk, [&] {
                    return m_stopping.load(std::memory_order_acquire) ||
                           (m_permits_used < m_opts.block_permits &&
                            f->permits_held < m_opts.per_file_cap);
                });
                if (m_stopping.load(std::memory_order_acquire)) break;
                m_permits_used++;
                f->permits_held++;
            }

            const bool hole =
                sparse_file && sparse::block_is_hole(fd, off, bsize);

            raw_block rb;
            rb.seq = seq;
            rb.index = idx;
            std::size_t consumed = bsize; // bytes of the file this block covers

            if (hole) {
                // Whole block inside a hole: never read it. The file-checksum
                // chain and (when enabled) the dedup identity still cover the
                // zero bytes, matching what reading zeros would produce.
                rb.sparse = true;
                rb.usize = static_cast<std::uint32_t>(bsize);
                fcrc = sparse::crc_extend_zeros(fcrc, bsize);
                rb.content_id = m_dedup ? sparse::zero_digest(bsize)
                                        : format::digest128{};
            } else {
                std::vector<std::uint8_t> raw(bsize);
                ssize_t n = arch::detail::retry_io(
                    [&] { return ::pread(fd, raw.data(), bsize,
                                         static_cast<off_t>(off)); });
                if (n <= 0) {
                    int e = errno;
                    std::lock_guard lk(m_mtx);
                    f->permits_held--; // no block was queued
                    m_permits_used--;
                    if (n < 0)
                        err = std::make_error_code(static_cast<std::errc>(e ? e : EIO));
                    break; // n == 0: file shrank — clean end of blocks
                }
                raw.resize(static_cast<std::size_t>(n));
                consumed = static_cast<std::size_t>(n);

                fcrc = crc32c::Extend(fcrc, raw.data(), raw.size());
                rb.content_id = m_dedup ? xxh3_128(raw) : format::digest128{};
                rb.data = std::move(raw);
            }

            {
                std::lock_guard lk(m_mtx);
                f->file_crc = fcrc; // keep in sync for the error path
                f->queued_raw++;
                m_raw_queue.push_back(std::move(rb));
            }
            m_cv.notify_all();

            off += static_cast<std::uint64_t>(consumed);
            idx++;
        }
        ::close(fd);

        {
            std::lock_guard lk(m_mtx);
            if (err) f->error = err;
            f->file_crc = fcrc;
            f->io_done = true;
            m_cv.notify_all();
        }
    }
}

// ---- Compression worker: single-block codec, results land in per-file slots.
// Every popped raw block owns one permit; on any drop path the permit is
// released exactly here (wait_file_done excludes in-flight blocks from its
// own reclaim). ----
void pipeline::compress_worker()
{
    std::unique_ptr<compress::compressor> comp;

    for (;;) {
        raw_block rb;
        std::shared_ptr<file_state> f;
        {
            std::unique_lock lk(m_mtx);
            m_cv.wait(lk, [this] {
                return m_stopping.load(std::memory_order_acquire) ||
                       !m_raw_queue.empty();
            });
            if (m_stopping.load(std::memory_order_acquire)) return;
            rb = std::move(m_raw_queue.front());
            m_raw_queue.pop_front();
            auto it = m_files.find(rb.seq);
            if (it == m_files.end() || it->second->error) {
                // File failed or was already reclaimed: drop the block.
                if (it != m_files.end()) {
                    it->second->queued_raw--;
                    it->second->permits_held--;
                }
                m_permits_used--;
                m_cv.notify_all();
                continue;
            }
            f = it->second;
            f->queued_raw--;
            f->in_compress++;
        }

        pipeline_block blk;
        blk.uncompressed_size = rb.sparse ? rb.usize
                                          : static_cast<std::uint32_t>(rb.data.size());
        blk.content_id = rb.content_id;
        try {
            if (rb.sparse) {
                // Hole block: 0 stored bytes; id says "expand to zeros".
                blk.compressed.clear();
                blk.crc32c = crc32c::Crc32c(blk.compressed.data(), 0);
                blk.compressor_id = format::COMPRESSOR_SPARSE;
            } else if (m_use_compression) {
                if (!comp) comp = compress::make_compressor(m_compressor, m_level);
                auto cb = comp->compress(rb.data);
                blk.compressed = std::move(cb.data);
                blk.crc32c = cb.crc32c;
                blk.compressor_id = cb.compressor_id;
            } else {
                blk.compressed = std::move(rb.data);
                blk.crc32c = crc32c::Crc32c(blk.compressed.data(), blk.compressed.size());
                blk.compressor_id = format::COMPRESSOR_NONE;
            }
        } catch (const std::exception& ex) {
            SPDLOG_ERROR("compression failed for {} block {}: {}",
                         f->path.c_str(), rb.index, ex.what());
            std::lock_guard lk(m_mtx);
            f->in_compress--;
            f->permits_held--;
            m_permits_used--;
            f->error = std::make_error_code(std::errc::io_error);
            m_cv.notify_all();
            continue;
        }

        std::lock_guard lk(m_mtx);
        f->in_compress--;
        auto it = m_files.find(rb.seq);
        if (it == m_files.end()) {
            m_permits_used--;          // file reclaimed; its permit was not ours to strand
        } else if (rb.index >= f->slots.size() || f->error) {
            // File failed while we compressed, or the slot vanished under a
            // shrink: drop the block and free its permit.
            f->permits_held--;
            m_permits_used--;
        } else {
            auto& s = f->slots[rb.index];
            s.blk = std::move(blk);
            s.ready = true;
        }
        m_cv.notify_all();
    }
}

} // namespace arch::io
