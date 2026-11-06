#include "arch/archive.h"
#include "arch/format.h"
#include "arch/compressor.h"
#include "arch/io_uring_pool.h"
#include "arch/prefetch.h"
#include "util/xattr.h"
#include "util/acl.h"
#include <spdlog/spdlog.h>
#include <stdexcept>
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

    // Dedup index
    struct dedup_node {
        std::uint64_t hash;
        std::uint64_t physical_offset;
        std::uint32_t ref_count;
    };
    std::vector<dedup_node> dedup_index;
    std::unordered_map<std::uint64_t, std::uint64_t> dedup_lookup; // hash → index into dedup_index

    writer::impl(options o, std::filesystem::path p)
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
        SPDLOG_ERROR("cannot create {}: {}", d->arc_path, strerror(errno));
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
        SPDLOG_ERROR("failed to write archive header for {}", d->arc_path);
        throw std::runtime_error("Failed to write archive header");
    }
}

writer::~writer() {
    if (d->arc_fd >= 0) ::close(d->arc_fd);
}

// Helper: compute xxHash3-64 of a buffer.
static std::uint64_t hash_block(std::span<const std::uint8_t> data) {
    return XXH3_64bits(data.data(), data.size());
}

// Helper: write a file's data using the prefetcher + thread pool compressors.
static void add_file_impl(writer::impl* d, std::filesystem::path src,
                          std::uint64_t file_size, int src_fd,
                          bool use_prefetch)
{
    auto comp = compress::make_compressor(d->opts.compressor);

    // Gather metadata
    struct stat st{};
    if (fstat(src_fd, &st)) {
        int e = errno;
        ::close(src_fd);
        SPDLOG_ERROR("fstat failed for {}: {}", src, strerror(e));
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
    entry.xattr_id = util::read_xattrs(src, d->xattr_data);
    entry.acl_id   = util::read_acl(src, d->acl_data);

    // Store path in string table
    entry.path_id = d->string_table.size();
    d->string_table += src.string();
    d->string_table.push_back('\0');

    // Block list start
    entry.block_list_offset = d->blocks.size();

    if (S_ISLNK(st.st_mode)) {
        // Symlink: read link target, don't archive data blocks
        std::string target = std::filesystem::read_symlink(src).string();
        // Stored in path_id already.
        entry.block_count = 0;
    } else if (S_ISREG(st.st_mode)) {
        // Regular file: read in blocks, compress in parallel.
        if (use_prefetch && d->opts.threads <= 1) {
            // Small-file path: use prefetcher for readahead.
        }

        // For now, simple block-by-block with thread pool for large files.
        std::uint64_t off = 0;
        crc32c::Crc32c file_crc;

        while (off < file_size) {
            std::uint64_t bsize = std::min(format::BLOCK_SIZE, file_size - off);

            // Read block (TODO: use io_uring submit_read in parallel).
            std::vector<std::uint8_t> raw(bsize);
            ssize_t n = ::pread(src_fd, raw.data(), bsize, off);
            if (n <= 0) {
                int e = errno;
                ::close(src_fd);
                SPDLOG_ERROR("pread failed for {} at offset {}: {}", src, off, strerror(e));
                throw std::system_error(e, std::generic_category(), "pread failed");
            }
            raw.resize(n);

            file_crc.Update(crc32c::StringView(reinterpret_cast<const char*>(raw.data()), raw.size()));

            // Dedup check
            std::uint64_t blk_hash = 0;
            if (d->opts.dedup) {
                blk_hash = hash_block(raw);
                auto hit = d->dedup_lookup.find(blk_hash);
                if (hit != d->dedup_lookup.end()) {
                    // Duplicate found — reuse existing block.
                    format::block_entry be{};
                    be.offset = d->dedup_index[hit->second].physical_offset;
                    be.content_id = blk_hash;
                    be.uncompressed_size = static_cast<std::uint32_t>(n);
                    // Copy compressor_id and crc from existing.
                    // (We look up the original block_entry for these.)
                    // For simplicity, re-lookup the physical block's entry.
                    d->dedup_index[hit->second].ref_count++;
                    d->blocks.push_back(be);
                    off += n;
                    continue;
                }
            }

            // Compress
            auto cb = comp->compress(raw);

            format::block_entry be{};
            be.offset = static_cast<std::uint64_t>(d->data_region.size());
            be.compressed_size   = static_cast<std::uint32_t>(cb.data.size());
            be.uncompressed_size = static_cast<std::uint32_t>(n);
            be.content_id        = blk_hash;
            be.crc32c           = cb.crc32c;
            be.compressor_id    = cb.compressor_id;

            d->data_region.insert(d->data_region.end(), cb.data.begin(), cb.data.end());
            d->blocks.push_back(be);

            if (d->opts.dedup) {
                d->dedup_lookup[blk_hash] = d->dedup_index.size();
                d->dedup_index.push_back({blk_hash, be.offset, 1});
            }

            off += n;
        }

        entry.checksum = file_crc.Value();
        entry.block_count = static_cast<std::uint64_t>(
            d->blocks.size() - entry.block_list_offset);
    } else {
        entry.block_count = 0;
        entry.checksum = 0;
    }

    ::close(src_fd);
    d->entries.push_back(entry);
}

void writer::add_file(std::filesystem::path path) {
    // Resolve to get real path + file size up-front.
    auto real = std::filesystem::absolute(path);
    struct stat st{};
    if (stat(real.c_str(), &st)) {
        int e = errno;
        SPDLOG_ERROR("stat failed for {}: {}", real, strerror(e));
        throw std::system_error(e, std::generic_category(), "stat failed");
    }

    int fd = ::open(real.c_str(), O_RDONLY);
    if (fd < 0) {
        int e = errno;
        SPDLOG_ERROR("open failed for {}: {}", real, strerror(e));
        throw std::system_error(e, std::generic_category(), "open failed");
    }

    bool use_prefetch = (st.st_size < (format::BLOCK_SIZE * 4)); // heuristic
    add_file_impl(d.get(), real, static_cast<std::uint64_t>(st.st_size),
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
    auto comp = compress::make_compressor(d->opts.compressor);
    entry.block_list_offset = d->blocks.size();

    std::size_t off = 0;
    crc32c::Crc32c file_crc;
    while (off < data.size()) {
        std::size_t bsize = std::min(format::BLOCK_SIZE, data.size() - off);
        auto chunk = std::span(reinterpret_cast<const std::uint8_t*>(data.data() + off), bsize);
        file_crc.Update(crc32c::StringView(data.data() + off, bsize));

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

    entry.checksum = file_crc.Value();
    entry.block_count = static_cast<std::uint64_t>(
        d->blocks.size() - entry.block_list_offset);

    d->entries.push_back(entry);
}

void writer::finalize() {
    // Write data region.
    if (!d->data_region.empty()) {
        std::uint64_t off = 16; // past archive header
        ::pwrite(d->arc_fd, d->data_region.data(), d->data_region.size(), off);
    }

    // Write string table.
    ::pwrite(d->arc_fd, d->string_table.data(), d->string_table.size(), 0); // offset TBD

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
        SPDLOG_ERROR("open failed for {}: {}", d->arc_path, strerror(e));
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
