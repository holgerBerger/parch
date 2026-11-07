// Phase 4: reader — footer/TOC deserialization, list(), extraction.
// Parsing is field-by-field per the docs/format.md v0.2 canonical offset
// tables (src/archive/format_parse.h); on-disk structs are never memcpy'd.
#include "arch/archive.h"
#include "arch/compressor.h"
#include "arch/format.h"
#include "format_parse.h"
#include "util/xattr.h"
#include <spdlog/spdlog.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <crc32c/crc32c.h>

namespace arch {

namespace fs = std::filesystem;

namespace {

constexpr std::uint16_t FORMAT_VERSION = 0x0002;

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

    ~impl() { if (fd >= 0) ::close(fd); }

    std::vector<std::uint8_t> read_at(std::uint64_t off, std::size_t n) const {
        std::vector<std::uint8_t> b(n);
        std::size_t got = 0;
        while (got < n) {
            ssize_t r = ::pread(fd, b.data() + got, n - got,
                                static_cast<off_t>(off + got));
            if (r < 0) {
                if (errno == EINTR) continue;
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
                      std::int64_t block_count, const std::filesystem::path& dest,
                      codec_cache& codecs) const;
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
void reader::impl::extract_data(const entry_location& loc, std::int64_t block_first,
                                std::int64_t block_count,
                                const std::filesystem::path& dest,
                                codec_cache& codecs) const
{
    const auto& sec = sections[loc.section];
    const auto& e = sec.entries[loc.index];

    int out = ::open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
        throw std::system_error(errno, std::generic_category(),
                                "cannot create " + dest.string());

    try {
        std::uint32_t file_crc = 0;
        std::uint64_t woff = 0;
        for (std::int64_t j = 0; j < block_count; ++j) {
            const auto& be = sec.blocks[static_cast<std::size_t>(block_first + j)];
            if (be.compressed_size == 0 || be.uncompressed_size > format::ARCH_BLOCK_SIZE)
                throw std::runtime_error("bogus block entry in " + dest.string());
            // block offsets are section-relative; data region is
            // [data_offset, data_offset + data_size).
            if (be.offset < sec.hdr.data_offset ||
                be.offset + be.compressed_size > sec.hdr.data_offset + sec.hdr.data_size)
                throw std::runtime_error("block past section data region in " + dest.string());
            auto raw_stored = read_at(sec.start + be.offset, be.compressed_size);
            if (crc32c::Crc32c(raw_stored.data(), raw_stored.size()) != be.crc32c)
                throw std::runtime_error("block CRC mismatch in " + dest.string());

            std::vector<std::uint8_t> plain;
            if (be.compressor_id == format::COMPRESSOR_NONE) {
                plain = std::move(raw_stored);
                if (plain.size() != be.uncompressed_size)
                    throw std::runtime_error("block size mismatch in " + dest.string());
            } else {
                plain = codecs.get(be.compressor_id)
                            .decompress(raw_stored, be.uncompressed_size);
                if (plain.size() != be.uncompressed_size)
                    throw std::runtime_error("decompression failed for " + dest.string());
            }

            std::size_t done = 0;
            while (done < plain.size()) {
                ssize_t w = ::pwrite(out, plain.data() + done, plain.size() - done,
                                     static_cast<off_t>(woff + done));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    int er = errno;
                    throw std::system_error(er, std::generic_category(),
                                            "pwrite failed for " + dest.string());
                }
                done += static_cast<std::size_t>(w);
            }
            file_crc = crc32c::Extend(file_crc, plain.data(), plain.size());
            woff += plain.size();
        }

        if (file_crc != e.checksum)
            throw std::runtime_error("file checksum mismatch for " + dest.string() +
                                     " (corrupt archive?)");
        if (woff != e.uncompressed_size && e.uncompressed_size != 0)
            SPDLOG_WARN("extracted size {} != recorded {} for {}", woff,
                        e.uncompressed_size, dest.c_str());
    } catch (...) {
        ::close(out);
        throw;
    }
    ::close(out);
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
        const auto target = zstr(strings(loc.section), e.symlink_target_id, "symlink target");
        fs::create_directories(dest.parent_path(), ec);
        fs::remove(dest, ec); // idempotent re-extract
        fs::create_symlink(target, dest, ec);
        if (ec) throw std::system_error(ec, "symlink " + dest.string());
        restore_metadata(e, dest, -1);
        return;
    }

    if (S_ISDIR(e.mode)) {
        fs::create_directories(dest, ec);
        if (ec) throw std::system_error(ec, "mkdir " + dest.string());
        return; // metadata applied in the final wave
    }

    if (S_ISREG(e.mode) || S_ISFIFO(e.mode)) {
        int out = -1;
        if (S_ISREG(e.mode)) {
            fs::create_directories(dest.parent_path(), ec);
            if (ec) throw std::system_error(ec, "mkdir " + dest.parent_path().string());
            // Hardlink duplicate: no own block list — resolve the canonical
            // owner entry (spec §File Entry) and store its data here.
            std::int64_t first = static_cast<std::int64_t>(e.block_list_offset);
            std::int64_t count = static_cast<std::int64_t>(e.block_count);
            if (e.link_target_id != 0) {
                const auto owner_name =
                    std::string(zstr(strings(loc.section), e.link_target_id, "hardlink target"));
                const auto& oloc = find_entry(owner_name);
                first = static_cast<std::int64_t>(sections[oloc.section].entries[oloc.index].block_list_offset);
                count = static_cast<std::int64_t>(sections[oloc.section].entries[oloc.index].block_count);
                extract_data(oloc, first, count, dest, codecs);
            } else {
                extract_data(loc, first, count, dest, codecs);
            }
            out = ::open(dest.c_str(), O_WRONLY | O_NOFOLLOW);
            if (out < 0)
                throw std::system_error(errno, std::generic_category(), "reopen " + dest.string());
        } else { // FIFO
            if (::mkfifo(dest.c_str(), (e.mode & 07777) | S_IFIFO) != 0 && errno != EEXIST)
                throw std::system_error(errno, std::generic_category(), "mkfifo " + dest.string());
        }

        restore_metadata(e, dest, out);

        // Xattr / ACL blobs (path-based; regular files only).
        if (S_ISREG(e.mode) && out >= 0) {
            if (e.xattr_id) {
                auto sv = strings(loc.section);
                util::write_xattrs(dest, sv.substr(e.xattr_id));
            }
            if (e.acl_id) {
                auto sv = strings(loc.section);
                // ACL blob is self-terminating (empty string closes it).
                util::write_acl(dest, sv.substr(e.acl_id));
            }
        }
        if (out >= 0) ::close(out);
        return;
    }

    // Block/char devices: best effort (needs privileges); sockets skipped.
    if (S_ISBLK(e.mode) || S_ISCHR(e.mode)) {
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

reader::reader(std::filesystem::path path, options opts)
    : d(std::make_unique<impl>())
{
    d->arc_path = std::move(path);
    d->opts = opts;
    d->open_and_parse();
}

reader::~reader() = default;

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

// Phase 4 item 23: extraction pipeline — per-file parallel read + decompress
// + write across *files* (same network-FS rationale as the write side).
// Waves (docs/api.md): directories → files/symlinks (pool) → hardlinks →
// directory metadata.
void reader::extract_all(std::filesystem::path dest) const {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) throw std::system_error(ec, "cannot create " + dest.string());

    // Unique jobs: last section wins for repeated paths (append archives);
    // duplicate-path earlier entries are fully overwritten anyway.
    std::vector<impl::entry_location> dirs, files, hardlinks;
    for (const auto& [path, loc] : d->by_path) {
        const auto& e = d->sections[loc.section].entries[loc.index];
        if (S_ISDIR(e.mode)) dirs.push_back(loc);
        else if (S_ISREG(e.mode) && e.link_target_id != 0) hardlinks.push_back(loc);
        else if (S_ISREG(e.mode) || S_ISLNK(e.mode) || S_ISFIFO(e.mode) ||
                 S_ISBLK(e.mode) || S_ISCHR(e.mode))
            files.push_back(loc);
        // (S_IFSOCK intentionally not recreated.)
    }

    // Wave 1: directories (+ parents serially so workers never mkdir-race).
    for (const auto& loc : dirs) {
        const auto& e = d->sections[loc.section].entries[loc.index];
        fs::create_directories(dest / std::string(
            zstr(d->strings(loc.section), e.path_id, "entry path")), ec);
        if (ec) throw std::system_error(ec, "mkdir " + dest.string());
    }

    // Wave 2: files/symlinks/devices on the pool.
    {
        int want = d->opts.threads > 0
                       ? d->opts.threads
                       : static_cast<int>(std::thread::hardware_concurrency());
        if (want < 1) want = 1;
        const auto nworkers = static_cast<std::size_t>(
            std::min<std::size_t>(static_cast<std::size_t>(want), files.size()));

        std::atomic<std::size_t> next{0};
        std::atomic<bool> stop{false};
        std::mutex err_mtx;
        std::exception_ptr first_err;

        auto worker = [&] {
            impl::codec_cache codecs; // one per thread; codec construction is lazy
            for (;;) {
                if (stop.load(std::memory_order_acquire)) return;
                const std::size_t i = next.fetch_add(1);
                if (i >= files.size()) return;
                try {
                    const auto& loc = files[i];
                    const auto& e = d->sections[loc.section].entries[loc.index];
                    const auto name = zstr(d->strings(loc.section), e.path_id, "entry path");
                    d->extract_one(loc, dest / std::filesystem::path(name), codecs);
                } catch (...) {
                    {
                        std::lock_guard lk(err_mtx);
                        if (!first_err) first_err = std::current_exception();
                    }
                    stop.store(true, std::memory_order_release);
                    return;
                }
            }
        };
        std::vector<std::thread> pool;
        pool.reserve(nworkers);
        for (std::size_t i = 0; i < nworkers; ++i) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
        if (first_err) std::rethrow_exception(first_err);
    }

    // Wave 3: hardlink duplicates — canonical owners now exist.
    {
        impl::codec_cache codecs;
        for (const auto& loc : hardlinks) {
            const auto& sec = d->sections[loc.section];
            const auto& e = sec.entries[loc.index];
            const auto name = zstr(d->strings(loc.section), e.path_id, "entry path");
            const auto owner = std::string(
                zstr(d->strings(loc.section), e.link_target_id, "hardlink target"));
            const auto owner_dest = dest / std::filesystem::path(owner);
            const auto target_dest = dest / std::filesystem::path(name);
            std::error_code rmec;
            fs::remove(target_dest, rmec);
            if (!fs::exists(owner_dest)) {
                // Canonical path was in a dropped/failed section: fall back
                // to extracting the owner's blocks under the duplicate name.
                const auto& oloc = d->find_entry(owner);
                d->extract_data(oloc,
                                static_cast<std::int64_t>(d->sections[oloc.section].entries[oloc.index].block_list_offset),
                                static_cast<std::int64_t>(d->sections[oloc.section].entries[oloc.index].block_count),
                                target_dest, codecs);
                continue;
            }
            if (::link(owner_dest.c_str(), target_dest.c_str()) != 0)
                throw std::system_error(errno, std::generic_category(),
                                        "hardlink " + target_dest.string());
        }
    }

    // Wave 4: directory metadata (children are all in place now).
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        const auto& loc = *it;
        const auto& sec = d->sections[loc.section];
        const auto& e = sec.entries[loc.index];
        const auto name = zstr(d->strings(loc.section), e.path_id, "entry path");
        const auto dpath = dest / std::filesystem::path(name);
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
    }
}

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
