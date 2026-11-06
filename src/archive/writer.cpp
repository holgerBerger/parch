#include "arch/archive.h"
#include "arch/format.h"
#include "arch/compressor.h"
#ifdef ARCH_WITH_URING
#include "arch/io_uring_pool.h"
#include "arch/prefetch.h"
#endif
#include "util/xattr.h"
#include "util/acl.h"
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <xxhash.h>
#include <cstring>
#include <crc32c/crc32c.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <algorithm>

namespace arch {

struct writer::impl {
    options opts;
    std::filesystem::path arc_path;
    int arc_fd{-1};

    // Running TOC section state
    std::vector<format::file_entry> entries;
    std::vector<format::block_entry> blocks;
    std::string string_table; // path strings concatenated, null-terminated
    std::vector<std::uint8_t> data_region; // compressed blocks appended here
    std::vector<std::uint64_t> xattr_blobs;  // offsets into a separate blob
    std::vector<std::uint8_t>  xattr_data;
    std::vector<std::uint64_t> acl_blobs;
    std::vector<std::uint8_t>  acl_data;

    // Dedup index (XXH3-128 identity, spec v0.2)
    struct dedup_node {
        format::digest128 hash;
        std::uint64_t physical_offset;
        std::uint64_t block_index;  // index into blocks[] of the owning entry
        std::uint32_t ref_count;
    };
    struct digest128_hash {
        std::size_t operator()(const format::digest128& d) const {
            std::uint64_t v;
            std::memcpy(&v, d.data(), sizeof v);
            return std::hash<std::uint64_t>{}(v);
        }
    };
    std::vector<dedup_node> dedup_index;
    std::unordered_map<format::digest128, std::uint64_t, digest128_hash>
        dedup_lookup; // hash → index into dedup_index

    /// Block pipeline for one already-opened file (src_fd owned by this call).
    /// Declared as a member because writer::impl is private to writer.
    void add_file_impl(std::filesystem::path src, std::uint64_t file_size,
                       int src_fd, bool use_prefetch);

    impl(options o, std::filesystem::path p)
        : opts(std::move(o)), arc_path(std::move(p))
    {
    }
};

writer::writer(std::filesystem::path path, options opts)
    : d(std::make_unique<impl>(std::move(opts), std::move(path)))
{
    d->arc_fd = ::open(d->arc_path.c_str(),
                       O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (d->arc_fd < 0) {
        SPDLOG_ERROR("cannot create {}: {}", d->arc_path.c_str(), strerror(errno));
        throw std::system_error(errno, std::generic_category(),
                               "cannot create " + d->arc_path.string());
    }

    // Write archive header magic
    std::array<std::uint8_t, 16> hdr{};
    hdr[0] = 'A', hdr[1] = 'R', hdr[2] = 'C', hdr[3] = 'H';
    hdr[4] = 0x01; // version
    // hdr[6..15] = 0
    if (::pwrite(d->arc_fd, hdr.data(), hdr.size(), 0) != hdr.size()) {
        ::close(d->arc_fd);
        SPDLOG_ERROR("failed to write archive header for {}", d->arc_path.c_str());
        throw std::runtime_error("Failed to write archive header");
    }
}

writer::~writer() {
    if (d->arc_fd >= 0) ::close(d->arc_fd);
}

// Helper: compute XXH3-128 of a buffer (v0.2 dedup identity).
static format::digest128 hash_block(std::span<const std::uint8_t> data) {
    XXH128_hash_t h = XXH3_128bits(data.data(), data.size());
    format::digest128 id{};
    std::memcpy(id.data(), &h.low64, 8);
    std::memcpy(id.data() + 8, &h.high64, 8);
    return id;
}

// Block pipeline for one already-opened file.
void writer::impl::add_file_impl(std::filesystem::path src,
                                 std::uint64_t file_size, int src_fd,
                                 bool use_prefetch)
{
    auto comp = compress::make_compressor(opts.compressor,
                                       opts.compression_level);

    // Gather metadata
    struct stat st{};
    if (fstat(src_fd, &st)) {
        int e = errno;
        ::close(src_fd);
        SPDLOG_ERROR("fstat failed for {}: {}", src.c_str(), strerror(e));
        throw std::system_error(e, std::generic_category(), "fstat failed");
    }

    format::file_entry entry{};
    entry.mode = static_cast<std::uint16_t>(st.st_mode);
    entry.uid  = st.st_uid;
    entry.gid  = st.st_gid;
    entry.mtime_sec  = static_cast<std::uint64_t>(st.st_mtim.tv_sec);
    entry.mtime_nsec = static_cast<std::uint32_t>(st.st_mtim.tv_nsec);
    entry.atime_sec  = static_cast<std::uint64_t>(st.st_atim.tv_sec);
    entry.atime_nsec = static_cast<std::uint32_t>(st.st_atim.tv_nsec);
    entry.nlink      = st.st_nlink;
    entry.uncompressed_size = st.st_size;

    // Xattr / ACL
    entry.xattr_id = util::read_xattrs(src, this->xattr_data);
    entry.acl_id   = util::read_acl(src, this->acl_data);

    // Store path in string table
    entry.path_id = this->string_table.size();
    this->string_table += src.string();
    this->string_table.push_back('\0');

    // Block list start
    entry.block_list_offset = this->blocks.size();

    if (S_ISLNK(st.st_mode)) {
        // Symlink: read link target, don't archive data blocks
        // TODO(Phase 1): store via file_entry::symlink_target_id once the
        // v0.2 layout lands; until then append the target to the string table
        // so it at least survives in the archive.
        std::uint64_t target_id = this->string_table.size();
        this->string_table += std::filesystem::read_symlink(src).string();
        this->string_table.push_back('\0');
        (void)target_id;
        entry.block_count = 0;
    } else if (S_ISREG(st.st_mode)) {
        // Regular file: read in blocks, compress in parallel.
        if (use_prefetch && this->opts.threads <= 1) {
            // Small-file path: use prefetcher for readahead.
        }

        // For now, simple block-by-block with thread pool for large files.
        std::uint64_t off = 0;
        std::uint32_t file_crc = 0;

        while (off < file_size) {
            std::uint64_t bsize = std::min(format::ARCH_BLOCK_SIZE, file_size - off);

            // Read block (TODO: use io_uring submit_read in parallel).
            std::vector<std::uint8_t> raw(bsize);
            ssize_t n = ::pread(src_fd, raw.data(), bsize, off);
            if (n <= 0) {
                int e = errno;
                ::close(src_fd);
                SPDLOG_ERROR("pread failed for {} at offset {}: {}", src.c_str(), off, strerror(e));
                throw std::system_error(e, std::generic_category(), "pread failed");
            }
            raw.resize(n);

            file_crc = crc32c::Extend(file_crc, raw.data(), raw.size());

            // Dedup check
            format::digest128 blk_hash{};
            if (this->opts.dedup) {
                blk_hash = hash_block(raw);
                auto hit = this->dedup_lookup.find(blk_hash);
                if (hit != this->dedup_lookup.end()) {
                    // Duplicate found — reuse existing block.
                    format::block_entry be{};
                    be.offset = this->dedup_index[hit->second].physical_offset;
                    be.content_id = blk_hash;
                    be.uncompressed_size = static_cast<std::uint32_t>(n);
                    // A dedup hit is fully self-describing: copy the owner's
                    // compressed_size / crc32c / compressor_id so the reader
                    // can resolve the block from block_entry alone (spec v0.2).
                    const auto& owner = this->blocks[
                        this->dedup_index[hit->second].block_index];
                    be.compressed_size = owner.compressed_size;
                    be.crc32c          = owner.crc32c;
                    be.compressor_id   = owner.compressor_id;
                    this->dedup_index[hit->second].ref_count++;
                    this->blocks.push_back(be);
                    off += n;
                    continue;
                }
            }

            // Compress
            auto cb = comp->compress(raw);

            format::block_entry be{};
            be.offset = static_cast<std::uint64_t>(this->data_region.size());
            be.compressed_size   = static_cast<std::uint32_t>(cb.data.size());
            be.uncompressed_size = static_cast<std::uint32_t>(n);
            be.content_id        = blk_hash;
            be.crc32c           = cb.crc32c;
            be.compressor_id    = cb.compressor_id;

            this->data_region.insert(this->data_region.end(), cb.data.begin(), cb.data.end());
            this->blocks.push_back(be);

            if (this->opts.dedup) {
                this->dedup_lookup[blk_hash] = this->dedup_index.size();
                this->dedup_index.push_back(
                    {blk_hash, be.offset, this->blocks.size() - 1, 1});
            }

            off += n;
        }

        entry.checksum = file_crc;
        entry.block_count = static_cast<std::uint64_t>(
            this->blocks.size() - entry.block_list_offset);
    } else {
        entry.block_count = 0;
        entry.checksum = 0;
    }

    ::close(src_fd);
    this->entries.push_back(entry);
}

void writer::add_file(std::filesystem::path path) {
    // Resolve to get real path + file size up-front.
    auto real = std::filesystem::absolute(path);
    struct stat st{};
    if (stat(real.c_str(), &st)) {
        int e = errno;
        SPDLOG_ERROR("stat failed for {}: {}", real.c_str(), strerror(e));
        throw std::system_error(e, std::generic_category(), "stat failed");
    }

    int fd = ::open(real.c_str(), O_RDONLY);
    if (fd < 0) {
        int e = errno;
        SPDLOG_ERROR("open failed for {}: {}", real.c_str(), strerror(e));
        throw std::system_error(e, std::generic_category(), "open failed");
    }

    bool use_prefetch = (st.st_size < (format::ARCH_BLOCK_SIZE * 4)); // heuristic
    d->add_file_impl(real, static_cast<std::uint64_t>(st.st_size),
                     fd, use_prefetch);
}

void writer::add_file(std::filesystem::path path, std::string_view data) {
    // Metadata-only entry (in-memory source).
    format::file_entry entry{};
    entry.mode = S_IFREG | 0644;
    entry.uncompressed_size = data.size();
    entry.path_id = d->string_table.size();
    d->string_table += path.string();
    d->string_table.push_back('\0');

    // Compress in blocks
    auto comp = compress::make_compressor(d->opts.compressor,
                                       d->opts.compression_level);
    entry.block_list_offset = d->blocks.size();

    std::size_t off = 0;
    std::uint32_t file_crc = 0;
    while (off < data.size()) {
        std::size_t bsize = std::min(format::ARCH_BLOCK_SIZE, data.size() - off);
        auto chunk = std::span(reinterpret_cast<const std::uint8_t*>(data.data() + off), bsize);
        file_crc = crc32c::Extend(file_crc,
                                  reinterpret_cast<const std::uint8_t*>(data.data() + off),
                                  bsize);

        auto cb = comp->compress(chunk);

        format::block_entry be{};
        be.offset = static_cast<std::uint64_t>(d->data_region.size());
        be.compressed_size   = static_cast<std::uint32_t>(cb.data.size());
        be.uncompressed_size = static_cast<std::uint32_t>(bsize);
        be.crc32c           = cb.crc32c;
        be.compressor_id    = cb.compressor_id;

        d->data_region.insert(d->data_region.end(), cb.data.begin(), cb.data.end());
        d->blocks.push_back(be);
        off += bsize;
    }

    entry.checksum = file_crc;
    entry.block_count = static_cast<std::uint64_t>(
        d->blocks.size() - entry.block_list_offset);

    d->entries.push_back(entry);
}

void writer::finalize() {
    // Write data region.
    std::uint64_t off = 16; // past archive header
    if (!d->data_region.empty()) {
        ::pwrite(d->arc_fd, d->data_region.data(), d->data_region.size(), off);
        off += d->data_region.size();
    }

    // Write string table after the data region (offset 0 clobbered the header).
    if (!d->string_table.empty()) {
        ::pwrite(d->arc_fd, d->string_table.data(), d->string_table.size(), off);
        off += d->string_table.size();
    }

    // Compute footer and write.
    // (Full serialization of TOC section + footer omitted in skeleton —
    //  this is a first-pass scaffold.)

    ::fsync(d->arc_fd);
    ::close(d->arc_fd);
    d->arc_fd = -1;
}

void writer::append(std::filesystem::path path) {
    // Open existing archive, read footer, append new TOC section at end.
    // (Skeleton — full impl reads footer, seeks past it, writes new section.)
}

// ---- reader ----

struct reader::impl {
    std::filesystem::path arc_path;
    int arc_fd{-1};
    format::footer m_footer{};
    std::vector<format::toc_section_ref> m_tocs;
    // Parsed entries across all TOC sections.
    std::vector<format::file_entry> m_entries;
    std::string m_string_table;
};

reader::reader(std::filesystem::path path)
    : d(std::make_unique<impl>())
{
    d->arc_path = std::move(path);
    d->arc_fd = ::open(d->arc_path.c_str(), O_RDONLY);
    if (d->arc_fd < 0) {
        int e = errno;
        SPDLOG_ERROR("open failed for {}: {}", d->arc_path.c_str(), strerror(e));
        throw std::system_error(e, std::generic_category(), "open failed");
    }

    // Parse footer, TOC refs, entries (skeleton).
}

reader::~reader() {
    if (d->arc_fd >= 0) ::close(d->arc_fd);
}

std::vector<reader::listing_entry> reader::list() const {
    // Build listing from parsed entries + string table.
    return {};
}

void reader::extract(const std::filesystem::path& arc_path,
                     std::filesystem::path dest) const {
    // Find entry, seek to blocks, decompress, write.
}

void reader::extract_all(std::filesystem::path dest) const {
}

std::vector<std::pair<std::string, std::vector<std::uint8_t>>>
reader::get_xattrs(const std::filesystem::path&) const {
    return {};
}

std::string reader::get_acl(const std::filesystem::path&) const {
    return {};
}

} // namespace arch
