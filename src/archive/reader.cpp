// Phase 4: reader — footer/TOC deserialization, list(), extraction.
// Parsing is field-by-field per the docs/format.md v0.2 canonical offset
// tables (src/archive/format_parse.h); on-disk structs are never memcpy'd.
#include "arch/archive.h"
#include "arch/compressor.h"
#include "arch/format.h"
#include "format_parse.h"
#include "io/sparse.h"
#include "util/retry.h"
#include "util/xattr.h"
#include <spdlog/spdlog.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <crc32c/crc32c.h>

namespace arch {

namespace fs = std::filesystem;

namespace {

constexpr std::uint16_t FORMAT_VERSION = format::FORMAT_VERSION_SPARSE; // accepts <= v0.3

// CRC+decompress concurrency gate (docs/extract-pipeline.md §3.2): file-pool
// workers must hold one permit while doing CRC + decompression (CPU-bound,
// options.threads wide) so latency-concurrency (options.depth workers) and
// CPU-concurrency stay independently tunable.
class decode_gate {
public:
    explicit decode_gate(int permits)
        : sem_(permits < 1 ? 1 : permits), permits_(permits < 1 ? 1 : permits) {}
    void acquire() { sem_.acquire(); }
    void release() { sem_.release(); }
    int permits() const { return permits_; }
private:
    std::counting_semaphore<> sem_;
    int permits_;
};

// Runs fn(i) for i in [0, njobs) on nworkers threads. On the first throw,
// records it, stops dispatching, joins, and returns it (callers rethrow).
template <class Fn>
std::exception_ptr run_jobs(std::size_t njobs, std::size_t nworkers, Fn&& fn)
{
    if (njobs == 0) return {};
    const auto nw = std::max<std::size_t>(1, std::min(nworkers, njobs));
    std::atomic<std::size_t> next{0};
    std::atomic<bool> stop{false};
    std::mutex m;
    std::exception_ptr err;
    std::vector<std::thread> pool;
    pool.reserve(nw);
    for (std::size_t w = 0; w < nw; ++w)
        pool.emplace_back([&] {
            for (;;) {
                if (stop.load(std::memory_order_acquire)) return;
                const std::size_t i = next.fetch_add(1);
                if (i >= njobs) return;
                try {
                    fn(i);
                } catch (...) {
                    std::lock_guard<std::mutex> lk(m);
                    if (!err) err = std::current_exception();
                    stop.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    for (auto& t : pool) t.join();
    return err;
}

std::string_view zstr(std::string_view table, std::uint64_t off, const char* what) {
    if (off == 0) return {}; // reserved NUL sentinel = "absent"
    if (off >= table.size())
        throw std::runtime_error(std::string(what) + ": string-table offset past end");
    const char* begin = table.data() + off;
    const char* endp = table.data() + table.size();
    const char* nul = static_cast<const char*>(std::memchr(begin, '\0', endp - begin));
    if (!nul) throw std::runtime_error(std::string(what) + ": unterminated string");
    return std::string_view(begin, static_cast<std::size_t>(nul - begin));
}

} // namespace

struct reader::impl {
    // (section, entry-index) pair into `sections`.
    struct entry_location {
        std::size_t section = 0;
        std::size_t index = 0;
    };

    struct section {
        std::uint64_t start = 0;           // absolute offset of the toc_header
        format::toc_header hdr{};
        std::vector<format::file_entry> entries;
        std::vector<format::block_entry> blocks;
        std::string strings;
    };

    std::filesystem::path arc_path;
    reader::options opts;
    int fd{-1};
    std::uint64_t file_size = 0;
    format::footer footer{};
    std::vector<section> sections;
    std::map<std::string, entry_location> by_path; // last section wins (append)

    // ---- Phase 10 (docs/extract-pipeline.md §3): instrumentation ----
    // Optional per-stage timers, enabled by options.profile. Off cost: one
    // bool test per stage (clock_gettime only when on). Counters are
    // cumulative across operations on this reader (extract_all, verify_all).
    struct stage_timer {
        std::atomic<std::uint64_t> ns{0};
        std::atomic<std::uint64_t> ops{0};
        void add(std::uint64_t dt) { ns.fetch_add(dt, std::memory_order_relaxed);
                                     ops.fetch_add(1, std::memory_order_relaxed); }
        void reset() { ns.store(0); ops.store(0); }
    };
    enum stage { STG_OPEN, STG_PREAD, STG_CRC, STG_DECODE, STG_PWRITE,
                 STG_META, STG_PERMIT_WAIT, STG_COUNT };
    static const char* stage_name(int s);
    mutable stage_timer stages[STG_COUNT];
    mutable std::atomic<std::uint64_t> wall_ns{0}, bytes_out{0}, files_done{0};

    // CRC+decompress permit pool = options.threads (CPU knob); the file
    // pool (options.depth) is the latency knob. Built by start_pool().
    std::unique_ptr<class decode_gate> gate;

    static std::uint64_t now_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
    }
    // RAII span: records elapsed ns when destroyed (if profiling on).
    class span {
        const impl* d_; int st_; std::uint64_t t0_;
    public:
        span(const impl* d, int st) : d_(d), st_(st),
            t0_(d->opts.profile ? impl::now_ns() : 0) {}
        ~span() { if (d_->opts.profile) d_->stages[st_].add(impl::now_ns() - t0_); }
        span(const span&) = delete; span& operator=(const span&) = delete;
    };

    ~impl() { if (fd >= 0) ::close(fd); }

    std::vector<std::uint8_t> read_at(std::uint64_t off, std::size_t n) const {
        std::vector<std::uint8_t> b(n);
        std::size_t got = 0;
        while (got < n) {
            // Transient network-FS errors (ESTALE/EIO/...) retried with
            // backoff (Phase 8 item 31).
            ssize_t r = arch::detail::retry_io([&] {
                return ::pread(fd, b.data() + got, n - got,
                               static_cast<off_t>(off + got)); });
            if (r < 0) {
                int e = errno;
                throw std::system_error(e, std::generic_category(),
                                        "pread failed on archive");
            }
            if (r == 0) throw std::runtime_error("archive truncated during read");
            got += static_cast<std::size_t>(r);
        }
        return b;
    }

    std::string_view strings(std::size_t sec) const {
        return std::string_view(sections[sec].strings);
    }

    void open_and_parse();
    void parse_sections(const std::vector<format::toc_section_ref>& refs);

    // extraction
    struct codec_cache {
        std::unordered_map<std::uint8_t, std::unique_ptr<compress::compressor>> by_id;
        compress::compressor& get(std::uint8_t id) {
            auto it = by_id.find(id);
            if (it != by_id.end()) return *it->second;
            auto c = compress::make_compressor_by_id(id, 1); // level irrelevant for decode
            return *by_id.emplace(id, std::move(c)).first->second;
        }
    };

    void extract_data(const entry_location& loc, std::int64_t block_first,
                      std::int64_t block_count, int out_fd,
                      const std::filesystem::path& dest,
                      codec_cache& codecs, bool sink = false) const;
    std::vector<std::string> verify_all() const;
    std::string profile_report() const;
    void restore_metadata(const format::file_entry& e,
                          const std::filesystem::path& dest, int fd_at_meta) const;
    const entry_location& find_entry(const std::string& path) const {
        auto it = by_path.find(path);
        if (it == by_path.end())
            throw std::runtime_error("no such archive entry: " + path);
        return it->second;
    }
    void extract_one(const entry_location& loc, const std::filesystem::path& dest,
                     codec_cache& codecs) const;
};

// ---- constructor: full validation ("data blocks are never read here") ----
void reader::impl::open_and_parse() {
    fd = ::open(arc_path.c_str(), O_RDONLY);
    if (fd < 0) {
        int e = errno;
        SPDLOG_ERROR("open failed for {}: {}", arc_path.c_str(), strerror(e));
        throw std::system_error(e, std::generic_category(),
                                "cannot open " + arc_path.string());
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        int e = errno;
        throw std::system_error(e, std::generic_category(), "fstat failed");
    }
    file_size = static_cast<std::uint64_t>(st.st_size);
    if (file_size < format::ARCHIVE_HEADER_SIZE + format::FOOTER_SIZE)
        throw std::runtime_error("file too small to be an archive: " + arc_path.string());

    // Footer (authoritative, at EOF-256).
    auto fbytes = read_at(file_size - format::FOOTER_SIZE, format::FOOTER_SIZE);
    footer = detail::parse_footer(fbytes.data());
    if (std::memcmp(footer.magic.data(), format::FOOTER_MAGIC.data(), 4) != 0)
        throw std::runtime_error("bad footer magic — not an archive or corrupt: " +
                                 arc_path.string());
    // signature_crc covers [0,248) with all stored fields in place.
    const std::uint32_t sig = crc32c::Crc32c(fbytes.data(), 248);
    if (sig != footer.signature_crc)
        throw std::runtime_error("footer signature CRC mismatch in " + arc_path.string());
    // footer_crc covers [0,44) ∪ [48,248).
    std::uint32_t fcrc = crc32c::Extend(0, fbytes.data(), 44);
    fcrc = crc32c::Extend(fcrc, fbytes.data() + 48, 200);
    if (fcrc != footer.footer_crc)
        throw std::runtime_error("footer self-check CRC mismatch in " + arc_path.string());
    if (footer.version > FORMAT_VERSION)
        throw std::runtime_error("unsupported archive format version " +
                                 std::to_string(footer.version));

    // TOC table + CRC.
    const std::uint64_t table_off = footer.toc_table_offset;
    const std::uint64_t footer_off = file_size - format::FOOTER_SIZE;
    if (table_off < format::ARCHIVE_HEADER_SIZE ||
        footer.toc_count > (footer_off - table_off) / format::TOC_SECTION_REF_SIZE)
        throw std::runtime_error("bogus TOC table in " + arc_path.string());
    const std::size_t table_len =
        static_cast<std::size_t>(footer.toc_count * format::TOC_SECTION_REF_SIZE);
    auto tbytes = read_at(table_off, table_len);
    if (crc32c::Crc32c(tbytes.data(), tbytes.size()) != footer.toc_table_crc)
        throw std::runtime_error("TOC table CRC mismatch in " + arc_path.string());

    std::vector<format::toc_section_ref> refs;
    refs.reserve(footer.toc_count);
    for (std::size_t i = 0; i < footer.toc_count; ++i)
        refs.push_back(detail::parse_toc_ref(tbytes.data() + i * format::TOC_SECTION_REF_SIZE));

    // Durable-append contract: a reference extending past the table/footer
    // region marks a clean truncation point — drop it and everything after.
    std::vector<format::toc_section_ref> kept;
    for (auto& r : refs) {
        if (r.offset < format::ARCHIVE_HEADER_SIZE || r.size < format::TOC_HEADER_SIZE ||
            r.offset > table_off || r.size > table_off - r.offset) {
            SPDLOG_WARN("{}: TOC section at {} (size {}) extends past the TOC table — "
                        "truncation point, dropping this and later sections",
                        arc_path.c_str(), r.offset, r.size);
            break;
        }
        kept.push_back(r);
    }
    parse_sections(kept);

    // Flatten: last occurrence of a path wins (append semantics).
    for (std::size_t s = 0; s < sections.size(); ++s)
        for (std::size_t i = 0; i < sections[s].entries.size(); ++i)
            by_path[std::string(zstr(strings(s), sections[s].entries[i].path_id,
                                     "entry path"))] = entry_location{s, i};
}

void reader::impl::parse_sections(const std::vector<format::toc_section_ref>& refs) {
    sections.reserve(refs.size());
    for (const auto& ref : refs) {
        auto hbytes = read_at(ref.offset, format::TOC_HEADER_SIZE);
        auto h = detail::parse_toc_header(hbytes.data());
        if (h.magic != ((static_cast<std::uint32_t>('T') << 24) |
                        (static_cast<std::uint32_t>('O') << 16) |
                        (static_cast<std::uint32_t>('C') << 8) |
                        static_cast<std::uint32_t>('1')))
            throw std::runtime_error("bad TOC section magic at offset " +
                                     std::to_string(ref.offset));

        // Bounds: metadata tables live between the data region and EOF-section.
        const std::uint64_t sz = ref.size;
        const auto le = [](std::uint64_t a, std::uint64_t b) { return a <= b; };
        if (h.data_offset < format::TOC_HEADER_SIZE || !le(h.data_offset, sz) ||
            h.data_size > sz - h.data_offset ||
            h.file_table_offset < h.data_offset + h.data_size ||
            h.entry_count > (sz - h.file_table_offset) / format::FILE_ENTRY_SIZE ||
            h.block_table_offset <
                h.file_table_offset + h.entry_count * format::FILE_ENTRY_SIZE ||
            h.block_count > (sz - h.block_table_offset) / format::BLOCK_ENTRY_SIZE ||
            h.string_table_offset <
                h.block_table_offset + h.block_count * format::BLOCK_ENTRY_SIZE ||
            h.string_table_size > sz - h.string_table_offset ||
            (h.dedup_index_offset == 0) != (h.dedup_index_size == 0) ||
            (h.dedup_index_size != 0 &&
             (h.dedup_index_offset < h.string_table_offset + h.string_table_size ||
              h.dedup_index_size > sz - h.dedup_index_offset)))
            throw std::runtime_error("TOC section header out of bounds at offset " +
                                     std::to_string(ref.offset));

        // Metadata CRC covers the header (with metadata_crc zeroed) plus the
        // contiguous tail of tables/strings/dedup. Data blocks NOT included.
        auto tail = read_at(ref.offset + h.file_table_offset,
                            static_cast<std::size_t>(sz - h.file_table_offset));
        {
            std::array<std::uint8_t, format::TOC_HEADER_SIZE> hb{};
            std::memcpy(hb.data(), hbytes.data(), hb.size());
            std::fill(hb.begin() + 92, hb.begin() + 96, std::uint8_t{0});
            std::uint32_t crc = crc32c::Crc32c(hb.data(), hb.size());
            crc = crc32c::Extend(crc, tail.data(), tail.size());
            if (crc != h.metadata_crc || crc != ref.metadata_crc)
                throw std::runtime_error("TOC metadata CRC mismatch in section at offset " +
                                         std::to_string(ref.offset));
        }

        section sec;
        sec.start = ref.offset;
        sec.hdr = h;
        const auto* tp = tail.data();
        const auto rel = [&](std::uint64_t off) {
            return tp + (off - h.file_table_offset);
        };
        for (std::uint64_t i = 0; i < h.entry_count; ++i)
            sec.entries.push_back(
                detail::parse_file_entry(rel(h.file_table_offset) + i * format::FILE_ENTRY_SIZE));
        for (std::uint64_t i = 0; i < h.block_count; ++i)
            sec.blocks.push_back(
                detail::parse_block_entry(rel(h.block_table_offset) + i * format::BLOCK_ENTRY_SIZE));
        sec.strings.assign(reinterpret_cast<const char*>(rel(h.string_table_offset)),
                           h.string_table_size);
        sections.push_back(std::move(sec));
    }
}

// ---- extraction ----
// Phase 10 (docs/extract-pipeline.md §3.2): `out_fd` is the caller-owned
// output descriptor (already opened O_WRONLY|O_CREAT|O_TRUNC) — extraction
// never re-opens a file it just wrote (removes one create-path RT per file).
// sink == true (verify): no fd, no pwrite; holes are not recreated.
void reader::impl::extract_data(const entry_location& loc, std::int64_t block_first,
                                std::int64_t block_count, int out_fd,
                                const std::filesystem::path& dest,
                                codec_cache& codecs, bool sink) const
{
    const auto& sec = sections[loc.section];
    const auto& e = sec.entries[loc.index];

    std::uint32_t file_crc = 0;
    std::uint64_t woff = 0;
    bool had_sparse = false;
    for (std::int64_t j = 0; j < block_count; ++j) {
        const auto& be = sec.blocks[static_cast<std::size_t>(block_first + j)];

        // v0.3 sparse (hole) block: zero stored bytes, expands to
        // uncompressed_size zeros. Never read; leave the output hole.
        if (be.compressor_id == format::COMPRESSOR_SPARSE) {
            if (footer.version < format::FORMAT_VERSION_SPARSE)
                throw std::runtime_error("sparse block in v" +
                    std::to_string(footer.version) + " archive: " + dest.string());
            if (be.compressed_size != 0 ||
                be.uncompressed_size > format::ARCH_BLOCK_SIZE)
                throw std::runtime_error("bogus sparse block entry in " +
                                         dest.string() + " (block " +
                                         std::to_string(j) + ")");
            {
                span s(this, STG_CRC);
                if (crc32c::Extend(0, nullptr, 0) != be.crc32c)
                    throw std::runtime_error("sparse block CRC mismatch in " +
                                             dest.string() + " (block " +
                                             std::to_string(j) + ")");
            }
            file_crc = io::sparse::crc_extend_zeros(file_crc, be.uncompressed_size);
            woff += be.uncompressed_size;
            had_sparse = true;
            continue;
        }
        if (be.compressed_size == 0 || be.uncompressed_size > format::ARCH_BLOCK_SIZE)
            throw std::runtime_error("bogus block entry in " + dest.string() +
                                     " (block " + std::to_string(j) + ")");
        // block offsets are section-relative; data region is
        // [data_offset, data_offset + data_size).
        if (be.offset < sec.hdr.data_offset ||
            be.offset + be.compressed_size > sec.hdr.data_offset + sec.hdr.data_size)
            throw std::runtime_error("block past section data region in " +
                                     dest.string() + " (block " +
                                     std::to_string(j) + ")");
        std::vector<std::uint8_t> raw_stored;
        {
            // Archive read stays OUTSIDE the decode permit: source-I/O
            // concurrency scales with depth (file pool), CPU with threads.
            span s(this, STG_PREAD);
            raw_stored = read_at(sec.start + be.offset, be.compressed_size);
        }

        std::vector<std::uint8_t> plain;
        {
            std::uint64_t t0 = opts.profile ? now_ns() : 0;
            gate->acquire();
            if (opts.profile)
                stages[STG_PERMIT_WAIT].add(now_ns() - t0);
            try {
                {
                    span s(this, STG_CRC);
                    if (crc32c::Crc32c(raw_stored.data(), raw_stored.size()) != be.crc32c)
                        throw std::runtime_error("block CRC mismatch in " +
                                                 dest.string() + " (block " +
                                                 std::to_string(j) + ")");
                }
                if (be.compressor_id == format::COMPRESSOR_NONE) {
                    plain = std::move(raw_stored);
                    if (plain.size() != be.uncompressed_size)
                        throw std::runtime_error("block size mismatch in " + dest.string());
                } else {
                    span s(this, STG_DECODE);
                    plain = codecs.get(be.compressor_id)
                                .decompress(raw_stored, be.uncompressed_size);
                    if (plain.size() != be.uncompressed_size)
                        throw std::runtime_error("decompression failed for " + dest.string());
                }
            } catch (...) {
                gate->release();
                throw;
            }
            gate->release();
        }

        if (!sink) {
            span s(this, STG_PWRITE);
            std::size_t done = 0;
            while (done < plain.size()) {
                ssize_t w = ::pwrite(out_fd, plain.data() + done, plain.size() - done,
                                     static_cast<off_t>(woff + done));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    int er = errno;
                    throw std::system_error(er, std::generic_category(),
                                            "pwrite failed for " + dest.string());
                }
                done += static_cast<std::size_t>(w);
            }
        }
        file_crc = crc32c::Extend(file_crc, plain.data(), plain.size());
        woff += plain.size();
    }

    if (file_crc != e.checksum)
        throw std::runtime_error("file checksum mismatch for " + dest.string() +
                                 " (corrupt archive?)");
    if (!sink) {
        // A trailing hole was never pwritten — extend the file so the last
        // block's zeros actually exist (interior holes already do: pwrite
        // past a hole leaves it unallocated).
        if (had_sparse &&
            ::ftruncate(out_fd, static_cast<off_t>(woff)) != 0)
            throw std::system_error(errno, std::generic_category(),
                                    "ftruncate failed for " + dest.string());
        if (woff != e.uncompressed_size && e.uncompressed_size != 0)
            SPDLOG_WARN("extracted size {} != recorded {} for {}", woff,
                        e.uncompressed_size, dest.c_str());
    }
    bytes_out.fetch_add(woff, std::memory_order_relaxed);
    files_done.fetch_add(1, std::memory_order_relaxed);
}

// Restore mode/ownership/times onto an extracted path.
// fd_at_meta: open fd for a regular file (utimens/fchown via fd), else -1.
void reader::impl::restore_metadata(const format::file_entry& e,
                                    const std::filesystem::path& dest,
                                    int fd_at_meta) const
{
    if (S_ISLNK(e.mode)) {
        // chmod/chown on symlinks are meaningless or unsupported; times only.
        struct timespec times[2] = {{static_cast<time_t>(e.atime_sec), e.atime_nsec},
                                    {static_cast<time_t>(e.mtime_sec), e.mtime_nsec}};
        ::utimensat(AT_FDCWD, dest.c_str(), times, AT_SYMLINK_NOFOLLOW);
        return;
    }

    if (fd_at_meta >= 0) {
        ::fchmod(fd_at_meta, e.mode & 07777);
        if (::fchown(fd_at_meta, e.uid, e.gid) != 0 && errno != EPERM && errno != EINVAL)
            SPDLOG_DEBUG("fchown failed for {}: {}", dest.c_str(), strerror(errno));
        struct timespec times[2] = {{static_cast<time_t>(e.atime_sec), e.atime_nsec},
                                    {static_cast<time_t>(e.mtime_sec), e.mtime_nsec}};
        ::futimens(fd_at_meta, times);
    } else {
        ::chmod(dest.c_str(), e.mode & 07777);
        if (::chown(dest.c_str(), e.uid, e.gid) != 0 && errno != EPERM && errno != EINVAL)
            SPDLOG_DEBUG("chown failed for {}: {}", dest.c_str(), strerror(errno));
        struct timespec times[2] = {{static_cast<time_t>(e.atime_sec), e.atime_nsec},
                                    {static_cast<time_t>(e.mtime_sec), e.mtime_nsec}};
        ::utimensat(AT_FDCWD, dest.c_str(), times, 0);
    }
}

void reader::impl::extract_one(const entry_location& loc,
                               const std::filesystem::path& dest,
                               codec_cache& codecs) const
{
    const auto& sec = sections[loc.section];
    const auto& e = sec.entries[loc.index];
    const auto name = zstr(strings(loc.section), e.path_id, "entry path");
    detail::check_member_name(name);
    std::error_code ec;

    if (S_ISLNK(e.mode)) {
        span s(this, STG_OPEN); // symlink(2) + remove are create-path RTs
        const auto target = zstr(strings(loc.section), e.symlink_target_id, "symlink target");
        fs::create_directories(dest.parent_path(), ec);
        fs::remove(dest, ec); // idempotent re-extract
        fs::create_symlink(target, dest, ec);
        if (ec) throw std::system_error(ec, "symlink " + dest.string());
        restore_metadata(e, dest, -1);
        return;
    }

    if (S_ISDIR(e.mode)) {
        span s(this, STG_OPEN);
        fs::create_directories(dest, ec);
        if (ec) throw std::system_error(ec, "mkdir " + dest.string());
        return; // metadata applied in the final wave
    }

    if (S_ISREG(e.mode) || S_ISFIFO(e.mode)) {
        // Phase 10 §3.2: ONE descriptor for data + metadata — the old code
        // wrote the file in extract_data, closed it, then re-opened it just
        // to fchmod/futimens, paying a second create-path RT per file.
        int out = -1;
        struct fd_guard {
            int& f;
            ~fd_guard() { if (f >= 0) ::close(f); }
        } guard{out};

        if (S_ISREG(e.mode)) {
            std::int64_t first = static_cast<std::int64_t>(e.block_list_offset);
            std::int64_t count = static_cast<std::int64_t>(e.block_count);
            entry_location data_loc = loc;
            // Hardlink duplicate: no own block list — resolve the canonical
            // owner entry (spec §File Entry) and store its data here.
            if (e.link_target_id != 0) {
                const auto owner_name =
                    std::string(zstr(strings(loc.section), e.link_target_id, "hardlink target"));
                data_loc = find_entry(owner_name);
                const auto& oe = sections[data_loc.section].entries[data_loc.index];
                first = static_cast<std::int64_t>(oe.block_list_offset);
                count = static_cast<std::int64_t>(oe.block_count);
            }
            {
                span s(this, STG_OPEN);
                fs::create_directories(dest.parent_path(), ec);
                if (ec) throw std::system_error(ec, "mkdir " + dest.parent_path().string());
                out = ::open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (out < 0)
                    throw std::system_error(errno, std::generic_category(),
                                            "cannot create " + dest.string());
            }
            // file_crc/size checks inside use data_loc's entry (owner's for
            // dups — identical content, matching the pre-Phase-10 behavior).
            extract_data(data_loc, first, count, out, dest, codecs);
        } else { // FIFO
            span s(this, STG_OPEN);
            if (::mkfifo(dest.c_str(), (e.mode & 07777) | S_IFIFO) != 0 && errno != EEXIST)
                throw std::system_error(errno, std::generic_category(), "mkfifo " + dest.string());
        }

        {
            span s(this, STG_META);
            restore_metadata(e, dest, out);
            // Xattr / ACL blobs (fd-based since Phase 10; regular files only).
            if (S_ISREG(e.mode) && out >= 0) {
                if (e.xattr_id)
                    util::write_xattrs(out, strings(loc.section).substr(e.xattr_id));
                if (e.acl_id) // access-only on fd (no default ACLs on files)
                    util::write_acl(out, strings(loc.section).substr(e.acl_id));
            }
        }
        return; // guard closes `out`
    }

    // Block/char devices: best effort (needs privileges); sockets skipped.
    if (S_ISBLK(e.mode) || S_ISCHR(e.mode)) {
        span s(this, STG_OPEN);
        fs::create_directories(dest.parent_path(), ec);
        if (::mknod(dest.c_str(), e.mode,
                    makedev(e.rdev_major, e.rdev_minor)) != 0)
            SPDLOG_WARN("mknod {} failed (need privileges?): {}", dest.c_str(), strerror(errno));
        else
            restore_metadata(e, dest, -1);
    } else {
        SPDLOG_WARN("skipping extraction of unsupported file type for {}", dest.c_str());
    }
}

// ---- public API ----

const char* reader::impl::stage_name(int st) {
    switch (st) {
        case STG_OPEN:        return "create+open ";
        case STG_PREAD:       return "archive read";
        case STG_CRC:         return "crc32c      ";
        case STG_DECODE:      return "decompress  ";
        case STG_PWRITE:      return "write out   ";
        case STG_META:        return "metadata    ";
        case STG_PERMIT_WAIT: return "permit wait";
        default:              return "?";
    }
}

reader::reader(std::filesystem::path path, options opts)
    : d(std::make_unique<impl>())
{
    d->arc_path = std::move(path);
    d->opts = opts;
    int permits = opts.threads > 0
                      ? opts.threads
                      : static_cast<int>(std::thread::hardware_concurrency());
    if (permits < 1) permits = 1;
    d->gate = std::make_unique<decode_gate>(permits);
    d->open_and_parse();
}

reader::~reader() = default;

std::string reader::profile_report() const { return d->profile_report(); }

// Human-readable per-stage breakdown + tuning hints (options.profile).
// "busy" = summed worker time across stages; wall vs busy* shows how much
// of the run was latency stalls rather than work — that pairing drives
// the depth/threads hints (docs/extract-pipeline.md §3, §7).
std::string reader::impl::profile_report() const
{
    if (!opts.profile) return {};
    std::string out;
    char line[256];
    std::uint64_t busy = 0;
    for (int i = 0; i < STG_COUNT; ++i) busy += stages[i].ns.load();
    const double wall = wall_ns.load() / 1e9;
    const double bytes = bytes_out.load();
    // Worker utilization: summed stage ns vs (wall ns × depth workers).
    const double util = wall_ns.load() > 0
        ? static_cast<double>(busy) /
              (static_cast<double>(wall_ns.load()) * std::max(1, opts.depth))
        : 0.0;
    std::snprintf(line, sizeof line,
                  "parch profile: %llu files, %.1f MiB written, %.3f s wall, "
                  "workers=%d permits=%d, busy=%.2f s (%.1f%% worker utilization)\n",
                  (unsigned long long)files_done.load(), bytes / (1 << 20), wall,
                  std::max(1, opts.depth), gate->permits(), busy / 1e9,
                  100.0 * util);
    out += line;
    for (int i = 0; i < STG_COUNT; ++i) {
        const auto ns = stages[i].ns.load();
        const auto ops = stages[i].ops.load();
        if (!ns && !ops) continue;
        std::snprintf(line, sizeof line,
                      "  %s %8.3f s %5.1f%% of busy  (%llu ops, %.1f us/op)\n",
                      stage_name(i), ns / 1e9, busy ? 100.0 * ns / busy : 0.0,
                      (unsigned long long)ops, ops ? ns / 1000.0 / ops : 0.0);
        out += line;
    }
    auto frac = [&](int st) { return busy ? static_cast<double>(stages[st].ns.load()) / busy : 0.0; };
    const double rt = frac(STG_OPEN) + frac(STG_META);
    const double cpu = frac(STG_CRC) + frac(STG_DECODE);
    std::uint64_t hints = 0;
    if (busy == 0 || wall == 0) return out;
    if (rt > 0.30) {
        std::snprintf(line, sizeof line,
                      "parch profile hint: output-side RTs (create+open+metadata) are %.0f%% "
                      "of worker time — latency-bound: raise --depth (currently %d)%s\n",
                      rt * 100.0, std::max(1, opts.depth),
                      opts.depth >= 128 ? " (already deep: the target FS's metadata "
                                        "rate is the floor; consider skipping metadata restore)"
                                        : "");
        out += line; ++hints;
    }
    if (frac(STG_PERMIT_WAIT) > 0.15) {
        std::snprintf(line, sizeof line,
                      "parch profile hint: workers wait %.0f%% of busy time on decode permits "
                      "— -j %d is the bottleneck: raise -j, or lower the compression level "
                      "to cut per-block CPU\n",
                      frac(STG_PERMIT_WAIT) * 100.0, gate->permits());
        out += line; ++hints;
    }
    if (cpu > 0.50) {
        std::snprintf(line, sizeof line,
                      "parch profile hint: CRC+decompress is %.0f%% of busy time — CPU-bound: "
                      "more -j (up to core count), or trade ratio for speed with a lower -l\n",
                      cpu * 100.0);
        out += line; ++hints;
    }
    if (frac(STG_PREAD) > 0.50) {
        std::snprintf(line, sizeof line,
                      "parch profile hint: archive reads are %.0f%% of busy time — source "
                      "bandwidth/latency-bound; one huge file = one cursor (block fan-out "
                      "needs the staged pipeline, docs/extract-pipeline.md §4)\n",
                      frac(STG_PREAD) * 100.0);
        out += line; ++hints;
    }
    if (frac(STG_PWRITE) > 0.40) {
        std::snprintf(line, sizeof line,
                      "parch profile hint: destination writes are %.0f%% of busy time — "
                      "target bandwidth-bound; page-cache hygiene (fadvise/DIRECT) is the "
                      "lever, not threads (docs/extract-pipeline.md §7.2)\n",
                      frac(STG_PWRITE) * 100.0);
        out += line; ++hints;
    }
    if (util < 0.25 && rt <= 0.30 && frac(STG_PERMIT_WAIT) <= 0.15) {
        std::snprintf(line, sizeof line,
                      "parch profile hint: workers are only %.0f%% busy and no stage dominates "
                      "— stalls sit between spans (kernel I/O waits): raise --depth (now %d)\n",
                      util * 100.0, std::max(1, opts.depth));
        out += line; ++hints;
    }
    if (hints == 0)
        out += "parch profile hint: no single dominant bottleneck\n";
    return out;
}

std::vector<reader::listing_entry> reader::list() const {
    std::vector<listing_entry> out;
    std::size_t n = 0;
    for (const auto& s : d->sections) n += s.entries.size();
    out.reserve(n);
    for (std::size_t si = 0; si < d->sections.size(); ++si) {
        const auto& sec = d->sections[si];
        const auto sv = d->strings(si);
        for (const auto& e : sec.entries) {
            listing_entry le;
            le.path = std::filesystem::path(std::string(zstr(sv, e.path_id, "entry path")));
            le.type = static_cast<format::file_type>(e.mode & 0170000);
            le.size = e.uncompressed_size;
            le.mode = e.mode;
            le.uid = e.uid;
            le.gid = e.gid;
            le.mtime = e.mtime_sec;
            if (e.symlink_target_id)
                le.link_target = std::string(zstr(sv, e.symlink_target_id, "symlink target"));
            else if (e.link_target_id)
                le.link_target = std::string(zstr(sv, e.link_target_id, "hardlink target"));
            out.push_back(std::move(le));
        }
    }
    return out;
}

void reader::extract(const std::filesystem::path& arc_path,
                     std::filesystem::path dest) const {
    impl::codec_cache codecs;
    d->extract_one(d->find_entry(arc_path.string()), dest, codecs);
}

// Phase 4 item 23 + Phase 10 (docs/extract-pipeline.md §3): extraction waves —
// dirs → files/symlinks/devices → hardlinks → dir metadata. Wave ORDER is
// still load-bearing; concurrency now has two knobs: the wave pools are
// options.depth workers wide (hides output-side open/metadata round-trips),
// while CRC+decompress runs gated to options.threads permits (CPU), and the
// archive `pread` sits outside the gate. Jobs iterate in archive LAYOUT
// order with by_path last-occurrence-wins dedup (sequential source reads).
void reader::extract_all(std::filesystem::path dest) const {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) throw std::system_error(ec, "cannot create " + dest.string());

    const std::size_t nworkers = static_cast<std::size_t>(std::max(1, d->opts.depth));
    const std::uint64_t t0 = d->opts.profile ? impl::now_ns() : 0;

    // Unique jobs in layout order: an entry runs iff it is the by_path
    // winner (earlier duplicates at the same path are overwritten anyway).
    std::vector<impl::entry_location> dirs, files, hardlinks;
    for (std::size_t si = 0; si < d->sections.size(); ++si) {
        const auto& sec = d->sections[si];
        for (std::size_t ei = 0; ei < sec.entries.size(); ++ei) {
            const auto& e = sec.entries[ei];
            const auto name = zstr(d->strings(si), e.path_id, "entry path");
            const auto it = d->by_path.find(std::string(name));
            if (it == d->by_path.end() || it->second.section != si ||
                it->second.index != ei)
                continue; // superseded by a later section
            const impl::entry_location loc{si, ei};
            if (S_ISDIR(e.mode)) dirs.push_back(loc);
            else if (S_ISREG(e.mode) && e.link_target_id != 0) hardlinks.push_back(loc);
            else if (S_ISREG(e.mode) || S_ISLNK(e.mode) || S_ISFIFO(e.mode) ||
                     S_ISBLK(e.mode) || S_ISCHR(e.mode))
                files.push_back(loc);
            // (S_IFSOCK intentionally not recreated.)
        }
    }

    // Wave 1: directories. Layout order guarantees parents-before-children,
    // but the pool does not preserve it, so bucket by path depth and run
    // the buckets in ascending order (parallel within a bucket ⇒ no
    // mkdir races, no missing parents).
    {
        std::map<int, std::vector<std::size_t>> buckets;
        for (std::size_t i = 0; i < dirs.size(); ++i) {
            const auto& loc = dirs[i];
            const auto& e = d->sections[loc.section].entries[loc.index];
            const auto nm = std::filesystem::path(
                std::string(zstr(d->strings(loc.section), e.path_id, "entry path")));
            int depth = 0;
            for (auto it = nm.begin(); it != nm.end(); ++it) ++depth;
            buckets[depth].push_back(i);
        }
        for (auto& [depth, idx] : buckets) {
            auto err = run_jobs(idx.size(), nworkers, [&](std::size_t k) {
                const auto& loc = dirs[idx[k]];
                const auto& e = d->sections[loc.section].entries[loc.index];
                const auto name = zstr(d->strings(loc.section), e.path_id, "entry path");
                impl::span s(&*d, impl::STG_OPEN);
                std::error_code mkec;
                fs::create_directories(dest / std::filesystem::path(name), mkec);
                if (mkec) throw std::system_error(mkec, "mkdir " + dest.string());
            });
            if (err) std::rethrow_exception(err);
        }
    }

    // Wave 2: files/symlinks/devices. extract_one opens the output file
    // once and keeps the fd through metadata restore (Phase 10 §3.2 step 1);
    // depth workers overlap the create-path RTs, threads gate the CPU part.
    if (auto err = run_jobs(files.size(), nworkers, [&](std::size_t i) {
            thread_local impl::codec_cache codecs; // per-worker, lazy
            const auto& loc = files[i];
            const auto& e = d->sections[loc.section].entries[loc.index];
            const auto name = zstr(d->strings(loc.section), e.path_id, "entry path");
            d->extract_one(loc, dest / std::filesystem::path(name), codecs);
        }))
        std::rethrow_exception(err);

    // Wave 3: hardlink duplicates — canonical owners now exist. link() is
    // one create-path RT each: pooled too.
    if (auto err = run_jobs(hardlinks.size(), nworkers, [&](std::size_t i) {
            const auto& loc = hardlinks[i];
            const auto& e = d->sections[loc.section].entries[loc.index];
            const auto name = zstr(d->strings(loc.section), e.path_id, "entry path");
            const auto owner = std::string(
                zstr(d->strings(loc.section), e.link_target_id, "hardlink target"));
            const auto owner_dest = dest / std::filesystem::path(owner);
            const auto target_dest = dest / std::filesystem::path(name);
            std::error_code rmec;
            {
                impl::span s(&*d, impl::STG_OPEN);
                fs::remove(target_dest, rmec);
            }
            if (!fs::exists(owner_dest)) {
                // Canonical path was in a dropped/failed section: fall back
                // to extracting the owner's blocks under the duplicate name.
                thread_local impl::codec_cache codecs;
                const auto& oloc = d->find_entry(owner);
                const int out = ::open(target_dest.c_str(),
                                       O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (out < 0)
                    throw std::system_error(errno, std::generic_category(),
                                            "cannot create " + target_dest.string());
                try {
                    d->extract_data(oloc,
                        static_cast<std::int64_t>(d->sections[oloc.section].entries[oloc.index].block_list_offset),
                        static_cast<std::int64_t>(d->sections[oloc.section].entries[oloc.index].block_count),
                        out, target_dest, codecs);
                } catch (...) { ::close(out); throw; }
                ::close(out);
                return;
            }
            impl::span s(&*d, impl::STG_META);
            if (::link(owner_dest.c_str(), target_dest.c_str()) != 0)
                throw std::system_error(errno, std::generic_category(),
                                        "hardlink " + target_dest.string());
            d->files_done.fetch_add(1, std::memory_order_relaxed);
        }))
        std::rethrow_exception(err);

    // Wave 4: directory metadata (children must be in place first; order
    // among directories is irrelevant — pool it). Reverse iteration keeps
    // the historical children-before-parents application order.
    if (auto err = run_jobs(dirs.size(), nworkers, [&](std::size_t i) {
            const auto& loc = dirs[dirs.size() - 1 - i];
            const auto& sec = d->sections[loc.section];
            const auto& e = sec.entries[loc.index];
            const auto name = zstr(d->strings(loc.section), e.path_id, "entry path");
            const auto dpath = dest / std::filesystem::path(name);
            impl::span s(&*d, impl::STG_META);
            ::chmod(dpath.c_str(), e.mode & 07777);
            if (::chown(dpath.c_str(), e.uid, e.gid) != 0 && errno != EPERM && errno != EINVAL)
                SPDLOG_DEBUG("chown dir {} failed: {}", dpath.c_str(), strerror(errno));
            struct timespec times[2] = {{static_cast<time_t>(e.atime_sec), e.atime_nsec},
                                        {static_cast<time_t>(e.mtime_sec), e.mtime_nsec}};
            ::utimensat(AT_FDCWD, dpath.c_str(), times, 0);
            if (e.xattr_id)
                util::write_xattrs(dpath, d->strings(loc.section).substr(e.xattr_id));
            if (e.acl_id) // access + (for dirs) default ACL
                util::write_acl(dpath, d->strings(loc.section).substr(e.acl_id));
        }))
        std::rethrow_exception(err);

    if (d->opts.profile) {
        d->wall_ns.fetch_add(impl::now_ns() - t0);
        auto report = d->profile_report();
        std::fputs(report.c_str(), stderr);
        std::fflush(stderr);
    }
}

// verify (Phase 8 item 32) + Phase 10: read + CRC + decompress every
// data-bearing entry into the sink — now on the same two-knob pool as
// extraction (depth workers, threads-gated CPU). Errors are recorded in
// per-job slots so the reported order matches the serial implementation.
std::vector<std::string> reader::impl::verify_all() const {
    struct job { std::size_t si, ei; std::string name; };
    std::vector<job> jobs;
    for (std::size_t si = 0; si < sections.size(); ++si)
        for (std::size_t ei = 0; ei < sections[si].entries.size(); ++ei) {
            const auto& e = sections[si].entries[ei];
            if (!S_ISREG(e.mode)) continue;
            jobs.push_back({si, ei,
                std::string(zstr(strings(si), e.path_id, "entry path"))});
        }

    std::vector<std::string> errs(jobs.size());
    const std::size_t nworkers = static_cast<std::size_t>(std::max(1, opts.depth));
    const std::uint64_t t0 = opts.profile ? now_ns() : 0;

    run_jobs(jobs.size(), nworkers, [&](std::size_t k) {
        const auto& j = jobs[k];
        try {
            const auto& e = sections[j.si].entries[j.ei];
            std::int64_t first = static_cast<std::int64_t>(e.block_list_offset);
            std::int64_t count = static_cast<std::int64_t>(e.block_count);
            entry_location dloc{j.si, j.ei};
            if (e.link_target_id != 0) { // hardlink: verify via owner blocks
                const auto owner = std::string(
                    zstr(strings(j.si), e.link_target_id, "hardlink target"));
                dloc = find_entry(owner);
                first = static_cast<std::int64_t>(
                    sections[dloc.section].entries[dloc.index].block_list_offset);
                count = static_cast<std::int64_t>(
                    sections[dloc.section].entries[dloc.index].block_count);
            }
            // sink: no output fd at all (the /dev/null open + pwrites are gone)
            thread_local impl::codec_cache codecs; // per-worker, lazy
            extract_data(dloc, first, count, /*out_fd=*/-1, j.name,
                         codecs, /*sink=*/true);
        } catch (const std::exception& ex) {
            errs[k] = j.name + ": " + ex.what();
        }
    });

    if (opts.profile) {
        wall_ns.fetch_add(now_ns() - t0);
        auto report = profile_report();
        std::fputs(report.c_str(), stderr);
        std::fflush(stderr);
    }

    std::vector<std::string> out;
    for (auto& s : errs)
        if (!s.empty()) out.push_back(std::move(s));
    return out;
}

std::vector<std::string> reader::verify_all() const { return d->verify_all(); }

std::vector<std::pair<std::string, std::vector<std::uint8_t>>>
reader::get_xattrs(const std::filesystem::path& arc_path) const {
    const auto& loc = d->find_entry(arc_path.string());
    const auto& sec = d->sections[loc.section];
    const auto& e = sec.entries[loc.index];
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> out;
    if (!e.xattr_id) return out;

    // docs/format.md §Xattr Storage: [len u32, name_len u16, name, vlen u32,
    // value] × N, terminated by len == 0. Self-terminating within the table.
    auto sv = d->strings(loc.section);
    std::size_t p = e.xattr_id;
    const std::size_t end = sv.size();
    const auto* b = reinterpret_cast<const std::uint8_t*>(sv.data());
    for (;;) {
        if (p + 4 > end) throw std::runtime_error("xattr blob truncated");
        std::uint32_t total_be, total;
        std::memcpy(&total_be, b + p, 4);
        total = format::betoh32_safe(total_be);
        if (total == 0) break;
        if (total < 10 || p + total > end) throw std::runtime_error("bogus xattr blob");
        std::uint16_t nlen_be, nlen;
        std::memcpy(&nlen_be, b + p + 4, 2);
        nlen = format::betoh16_safe(nlen_be);
        if (nlen == 0 || 6u + nlen + 4u > total) throw std::runtime_error("bogus xattr name");
        std::string name(sv.data() + p + 6, nlen);
        std::uint32_t vlen_be, vlen;
        std::memcpy(&vlen_be, b + p + 6 + nlen, 4);
        vlen = format::betoh32_safe(vlen_be);
        if (6u + nlen + 4u + vlen > total) throw std::runtime_error("bogus xattr value");
        out.emplace_back(std::move(name),
                         std::vector<std::uint8_t>(b + p + 6 + nlen + 4,
                                                   b + p + 6 + nlen + 4 + vlen));
        p += total;
    }
    return out;
}

std::string reader::get_acl(const std::filesystem::path& arc_path) const {
    const auto& loc = d->find_entry(arc_path.string());
    const auto& e = d->sections[loc.section].entries[loc.index];
    if (!e.acl_id) return {};
    const auto sv = d->strings(loc.section);
    if (e.acl_id >= sv.size())
        throw std::runtime_error("ACL text: string-table offset past end");
    // Blob = access NUL [default NUL] (+terminator); render getfacl-style
    // with "default:" prefixes on the second part.
    std::string out;
    std::string_view p = sv.substr(e.acl_id);
    for (bool first_part = true; !p.empty(); first_part = false) {
        const auto nul = p.find('\0');
        const std::string_view text =
            nul == std::string_view::npos ? p : p.substr(0, nul);
        if (text.empty()) break;
        if (first_part) {
            out += text;
        } else {
            out += '\n';
            std::string_view line = text;
            while (!line.empty()) {
                const auto nl = line.find('\n');
                const std::size_t len = nl == std::string_view::npos ? line.size() : nl;
                if (len) { out += "default:"; out.append(line.data(), len); out += '\n'; }
                if (nl == std::string_view::npos) break;
                line = line.substr(nl + 1);
            }
            if (!out.empty() && out.back() == '\n') out.pop_back();
        }
        if (nul == std::string_view::npos) break;
        p = p.substr(nul + 1);
    }
    return out;
}

} // namespace arch
