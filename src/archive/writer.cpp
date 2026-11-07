#include "arch/archive.h"
#include "arch/format.h"
#include "arch/compressor.h"
#include "arch/pipeline.h"
#include "format_parse.h"
#ifdef ARCH_WITH_URING
#include "arch/io_uring_pool.h"
#include "arch/prefetch.h"
#endif
#include "util/xattr.h"
#include "util/acl.h"
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <map>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <xxhash.h>
#include <cstring>
#include <cstdlib>
#include <crc32c/crc32c.h>
#include <algorithm>

namespace arch {

namespace {

// ---- big-endian field serialization (docs/format.md §Serialization rules) ----
// Structs are NEVER memcpy'd; every record is written field-by-field per the
// canonical offset tables. On BE hosts the htobe*_safe helpers are identity,
// so a memcpy of the converted value emits the same big-endian bytes.

template <typename T>
void put_be(std::vector<std::uint8_t>& b, T v);

template <>
void put_be<std::uint16_t>(std::vector<std::uint8_t>& b, std::uint16_t v) {
    auto x = format::htobe16_safe(v);
    const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
    b.insert(b.end(), p, p + 2);
}
template <>
void put_be<std::uint32_t>(std::vector<std::uint8_t>& b, std::uint32_t v) {
    auto x = format::htobe32_safe(v);
    const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
    b.insert(b.end(), p, p + 4);
}
template <>
void put_be<std::uint64_t>(std::vector<std::uint8_t>& b, std::uint64_t v) {
    auto x = format::htobe64_safe(v);
    const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
    b.insert(b.end(), p, p + 8);
}

void put_raw(std::vector<std::uint8_t>& b, const void* p, std::size_t n) {
    const auto* q = static_cast<const std::uint8_t*>(p);
    b.insert(b.end(), q, q + n);
}
void put_zeros(std::vector<std::uint8_t>& b, std::size_t n) {
    b.insert(b.end(), n, 0);
}

// file_entry — canonical layout, docs/format.md §File Entry (128 B).
std::vector<std::uint8_t> serialize_file_entry(const format::file_entry& e) {
    std::vector<std::uint8_t> b;
    b.reserve(format::FILE_ENTRY_SIZE);
    put_be<std::uint64_t>(b, e.path_id);            // 0
    put_be<std::uint64_t>(b, e.symlink_target_id);  // 8
    put_be<std::uint64_t>(b, e.link_target_id);     // 16
    put_be<std::uint16_t>(b, e.mode);               // 24
    put_be<std::uint32_t>(b, e.uid);                // 26
    put_be<std::uint32_t>(b, e.gid);                // 30
    put_be<std::uint64_t>(b, e.mtime_sec);          // 34
    put_be<std::uint32_t>(b, e.mtime_nsec);         // 42
    put_be<std::uint64_t>(b, e.atime_sec);          // 46
    put_be<std::uint32_t>(b, e.atime_nsec);         // 54
    put_be<std::uint64_t>(b, e.nlink);              // 58
    put_be<std::uint64_t>(b, e.uncompressed_size);  // 66
    put_be<std::uint64_t>(b, e.block_count);        // 74
    put_be<std::uint64_t>(b, e.block_list_offset);  // 82
    put_be<std::uint64_t>(b, e.xattr_id);           // 90
    put_be<std::uint64_t>(b, e.acl_id);             // 98
    put_be<std::uint32_t>(b, e.checksum);           // 106
    put_be<std::uint32_t>(b, e.rdev_major);         // 110
    put_be<std::uint32_t>(b, e.rdev_minor);         // 114
    put_zeros(b, 10);                               // 118 reserved
    if (b.size() != format::FILE_ENTRY_SIZE) std::abort();
    return b;
}

// block_entry — canonical layout, docs/format.md §Block Entry (48 B).
std::vector<std::uint8_t> serialize_block_entry(const format::block_entry& e) {
    std::vector<std::uint8_t> b;
    b.reserve(format::BLOCK_ENTRY_SIZE);
    put_be<std::uint64_t>(b, e.offset);              // 0
    put_be<std::uint32_t>(b, e.compressed_size);     // 8
    put_be<std::uint32_t>(b, e.uncompressed_size);   // 12
    put_raw(b, e.content_id.data(), 16);             // 16 opaque XXH3-128
    put_be<std::uint32_t>(b, e.crc32c);              // 32
    b.push_back(e.compressor_id);                    // 36
    put_zeros(b, 11);                                // 37 reserved
    if (b.size() != format::BLOCK_ENTRY_SIZE) std::abort();
    return b;
}

// dedup_entry — docs/format.md §Block Hash Index (32 B).
std::vector<std::uint8_t> serialize_dedup_entry(const format::dedup_entry& e) {
    std::vector<std::uint8_t> b;
    b.reserve(format::DEDUP_ENTRY_SIZE);
    put_raw(b, e.content_hash.data(), 16);           // 0
    put_be<std::uint64_t>(b, e.physical_offset);     // 16
    put_be<std::uint32_t>(b, e.reference_count);     // 24
    put_be<std::uint32_t>(b, e.reserved);            // 28
    if (b.size() != format::DEDUP_ENTRY_SIZE) std::abort();
    return b;
}

// toc_header — docs/format.md §TOC section header (128 B).
std::vector<std::uint8_t> serialize_toc_header(const format::toc_header& h) {
    std::vector<std::uint8_t> b;
    b.reserve(format::TOC_HEADER_SIZE);
    put_be<std::uint32_t>(b, h.magic);                 // 0 "TOC1" big-endian
    put_be<std::uint64_t>(b, h.entry_count);           // 4
    put_be<std::uint64_t>(b, h.block_count);           // 12
    put_be<std::uint64_t>(b, h.file_table_offset);     // 20
    put_be<std::uint64_t>(b, h.block_table_offset);    // 28
    put_be<std::uint64_t>(b, h.data_offset);           // 36
    put_be<std::uint64_t>(b, h.data_size);             // 44
    put_be<std::uint64_t>(b, h.string_table_offset);   // 52
    put_be<std::uint64_t>(b, h.string_table_size);     // 60
    put_be<std::uint64_t>(b, h.dedup_index_offset);    // 68
    put_be<std::uint64_t>(b, h.dedup_index_size);      // 76
    b.push_back(h.compressor_id);                      // 84
    b.push_back(h.reserved0);                          // 85
    put_be<std::uint16_t>(b, static_cast<std::uint16_t>(h.compression_level)); // 86
    put_be<std::uint32_t>(b, h.section_flags);         // 88
    put_be<std::uint32_t>(b, h.metadata_crc);          // 92
    put_zeros(b, 32);                                  // 96 reserved
    if (b.size() != format::TOC_HEADER_SIZE) std::abort();
    return b;
}

// toc_section_ref — TOC table record (24 B).
std::vector<std::uint8_t> serialize_toc_ref(const format::toc_section_ref& r) {
    std::vector<std::uint8_t> b;
    b.reserve(format::TOC_SECTION_REF_SIZE);
    put_be<std::uint64_t>(b, r.offset);        // 0
    put_be<std::uint64_t>(b, r.size);          // 8
    put_be<std::uint32_t>(b, r.metadata_crc);  // 16
    put_be<std::uint32_t>(b, r.reserved);      // 20
    if (b.size() != format::TOC_SECTION_REF_SIZE) std::abort();
    return b;
}

constexpr std::uint32_t TOC_MAGIC_U32 = // "TOC1" as big-endian bytes
    (static_cast<std::uint32_t>('T') << 24) | (static_cast<std::uint32_t>('O') << 16) |
    (static_cast<std::uint32_t>('C') << 8)  |  static_cast<std::uint32_t>('1');

} // namespace

struct writer::impl {
    options opts;
    std::filesystem::path arc_path;
    int arc_fd{-1};
    bool finalized{false};

    // ---- current TOC section state (RAM: metadata only, data is streamed) ----
    bool section_open = false;
    std::uint64_t section_start = 0;  // absolute file offset of toc_header
    std::uint64_t data_written = 0;   // compressed bytes streamed into section
    std::uint64_t file_end = 0;       // running EOF (we never lseek; pwrite only)

    static constexpr std::uint64_t DATA_OFFSET = format::TOC_HEADER_SIZE; // 128

    std::vector<format::file_entry> entries;
    std::vector<format::block_entry> blocks;
    std::string string_table; // byte 0 = reserved NUL sentinel (0 = "absent")

    // Dedup index (XXH3-128 identity, within-section only — frozen decision).
    struct dedup_node {
        format::digest128 hash;
        std::uint64_t physical_offset;  // section-relative
        std::uint64_t block_index;      // index into blocks[] of the owning entry
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

    // Section default codec (id pinned into toc_header; blocks carry their own).
    std::unique_ptr<compress::compressor> comp;
    std::uint8_t section_compressor_id = format::COMPRESSOR_NONE;

    // Phase 3 file-level I/O pipeline (lazily created by add_tree).
    std::unique_ptr<io::pipeline> pipe;

    // ---- archive-wide state ----
    std::vector<format::toc_section_ref> toc_refs; // finalized sections
    std::uint64_t total_uncompressed = 0;
    std::uint64_t total_compressed = 0;
    // Footer flags inherited from an existing archive via writer::append()
    // (OR-ed into the rewritten footer so DEDUP stays sticky).
    std::uint16_t footer_flags_inherited = 0;

    // Hardlink detection across the whole archive: (dev, ino) → first-relative-path.
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> inode_paths;

    impl(options o, std::filesystem::path p)
        : opts(std::move(o)), arc_path(std::move(p))
    {
    }

    /// Block pipeline for one already-opened file; takes ownership of src_fd.
    void add_file_impl(const std::filesystem::path& name,
                       const std::filesystem::path& full,
                       std::uint64_t file_size, int src_fd);

    /// Metadata-only entry (directories, devices, FIFOs, sockets, symlinks,
    /// hardlink duplicates). `override_mode` may carry the S_IFMT bits.
    void add_metadata_entry(const std::filesystem::path& name,
                            const std::filesystem::path& full,
                            const struct stat& st,
                            std::uint64_t symlink_target_id = 0,
                            std::uint64_t link_target_id = 0,
                            std::uint64_t uncompressed_size_override = 0);

    /// Lazily create the Phase 3 file-level I/O + compression pipeline.
    io::pipeline* get_pipeline();

    /// Drain one pipeline file in submit order: stream its blocks to disk
    /// (with dedup), then build the file_entry from the terminal record.
    void consume_pipelined_file(const std::string& rel, std::uint64_t seq);

    void ensure_section();
    void finalize_section();
    void write_footer();

    std::uint64_t add_string(std::string_view s) {
        std::uint64_t off = string_table.size();
        string_table += s;
        string_table.push_back('\0');
        return off;
    }
    std::uint64_t add_blob(const std::vector<std::uint8_t>& blob) {
        std::uint64_t off = string_table.size();
        string_table.append(reinterpret_cast<const char*>(blob.data()), blob.size());
        return off;
    }

    void write_at(std::uint64_t off, const void* p, std::size_t n);
    void sync_now() {
        if (opts.durable) ::fsync(arc_fd);
    }
};

void writer::impl::write_at(std::uint64_t off, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    while (n > 0) {
        ssize_t w = ::pwrite(arc_fd, b, n, static_cast<off_t>(off));
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            SPDLOG_ERROR("pwrite at {} failed: {}", off, strerror(e));
            throw std::system_error(e, std::generic_category(), "pwrite failed");
        }
        b += w;
        off += static_cast<std::uint64_t>(w);
        n -= static_cast<std::size_t>(w);
    }
}

void writer::impl::ensure_section() {
    if (section_open) return;
    // Reserve the toc_header slot; it is patched in finalize_section().
    section_start = file_end;
    const std::vector<std::uint8_t> reserved(format::TOC_HEADER_SIZE, 0);
    write_at(section_start, reserved.data(), reserved.size());
    file_end = section_start + format::TOC_HEADER_SIZE;

    data_written = 0;
    entries.clear();
    blocks.clear();
    dedup_index.clear();
    dedup_lookup.clear();
    string_table.assign(1, '\0'); // byte 0 sentinel: offset 0 = "absent"
    section_open = true;
}

// Helper: compute XXH3-128 of a buffer (v0.2 dedup identity).
static format::digest128 hash_block(std::span<const std::uint8_t> data) {
    XXH128_hash_t h = XXH3_128bits(data.data(), data.size());
    format::digest128 id{};
    std::memcpy(id.data(), &h.low64, 8);
    std::memcpy(id.data() + 8, &h.high64, 8);
    return id;
}

// Block pipeline for one already-opened file. Streams every compressed block
// straight to disk (spec §Creation flow step 3) — only metadata is buffered.
void writer::impl::add_file_impl(const std::filesystem::path& name,
                                 const std::filesystem::path& full,
                                 std::uint64_t file_size, int src_fd)
{
    if (!comp) {
        comp = compress::make_compressor(opts.compressor, opts.compression_level);
        section_compressor_id = comp->id();
    }

    struct stat st{};
    if (fstat(src_fd, &st)) {
        int e = errno;
        ::close(src_fd);
        SPDLOG_ERROR("fstat failed for {}: {}", full.c_str(), strerror(e));
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
    entry.path_id = add_string(name.string());
    entry.block_list_offset = blocks.size();

    // Xattr / ACL blobs live in the string table (xattr_id/acl_id = offset).
    {
        std::vector<std::uint8_t> blob;
        util::read_xattrs(full, blob);
        if (!blob.empty()) entry.xattr_id = add_blob(blob);
    }
    {
        std::vector<std::uint8_t> blob;
        util::read_acl(full, blob);
        if (!blob.empty()) {
            blob.push_back('\0'); // ACL text is NUL-terminated
            entry.acl_id = add_blob(blob);
        }
    }

    std::uint64_t off = 0;
    std::uint32_t file_crc = 0;

    try {
        while (off < file_size) {
            std::uint64_t bsize = std::min(format::ARCH_BLOCK_SIZE, file_size - off);

            std::vector<std::uint8_t> raw(bsize);
            ssize_t n = ::pread(src_fd, raw.data(), bsize, off);
            if (n <= 0) {
                int e = errno;
                SPDLOG_ERROR("pread failed for {} at offset {}: {}",
                             full.c_str(), off, strerror(e));
                throw std::system_error(e, std::generic_category(), "pread failed");
            }
            raw.resize(n);

            file_crc = crc32c::Extend(file_crc, raw.data(), raw.size());

            // Dedup check (within-section, spec §Deduplication).
            format::digest128 blk_hash{};
            if (opts.dedup) {
                blk_hash = hash_block(raw);
                auto hit = dedup_lookup.find(blk_hash);
                if (hit != dedup_lookup.end()) {
                    // Duplicate: self-describing copy of the owner's fields.
                    const auto& owner = blocks[dedup_index[hit->second].block_index];
                    format::block_entry be{};
                    be.offset            = owner.offset;
                    be.compressed_size   = owner.compressed_size;
                    be.uncompressed_size = static_cast<std::uint32_t>(n);
                    be.content_id        = blk_hash;
                    be.crc32c            = owner.crc32c;
                    be.compressor_id     = owner.compressor_id;
                    dedup_index[hit->second].ref_count++;
                    blocks.push_back(be);
                    off += n;
                    continue;
                }
            }

            // Compress and stream the block to disk immediately.
            auto cb = comp->compress(raw);

            format::block_entry be{};
            be.offset = DATA_OFFSET + data_written; // section-relative
            write_at(section_start + be.offset, cb.data.data(), cb.data.size());
            data_written += cb.data.size();

            be.compressed_size   = static_cast<std::uint32_t>(cb.data.size());
            be.uncompressed_size = static_cast<std::uint32_t>(n);
            be.content_id        = blk_hash;
            be.crc32c            = cb.crc32c;
            be.compressor_id     = cb.compressor_id;
            blocks.push_back(be);

            if (opts.dedup) {
                dedup_lookup[blk_hash] = dedup_index.size();
                dedup_index.push_back(
                    {blk_hash, be.offset, blocks.size() - 1, 1});
            }

            off += n;
        }
    } catch (...) {
        ::close(src_fd);
        throw;
    }
    ::close(src_fd);

    entry.checksum = file_crc;
    entry.block_count = blocks.size() - entry.block_list_offset;
    total_uncompressed += entry.uncompressed_size;
    entries.push_back(entry);
}

void writer::impl::add_metadata_entry(const std::filesystem::path& name,
                                      const std::filesystem::path& full,
                                      const struct stat& st,
                                      std::uint64_t symlink_target_id,
                                      std::uint64_t link_target_id,
                                      std::uint64_t uncompressed_size_override)
{
    format::file_entry entry{};
    entry.mode = static_cast<std::uint16_t>(st.st_mode);
    entry.uid  = st.st_uid;
    entry.gid  = st.st_gid;
    entry.mtime_sec  = static_cast<std::uint64_t>(st.st_mtim.tv_sec);
    entry.mtime_nsec = static_cast<std::uint32_t>(st.st_mtim.tv_nsec);
    entry.atime_sec  = static_cast<std::uint64_t>(st.st_atim.tv_sec);
    entry.atime_nsec = static_cast<std::uint32_t>(st.st_atim.tv_nsec);
    entry.nlink      = st.st_nlink;
    entry.path_id = add_string(name.string());
    entry.symlink_target_id = symlink_target_id;
    entry.link_target_id    = link_target_id;
    entry.uncompressed_size = uncompressed_size_override
                                  ? uncompressed_size_override : st.st_size;
    entry.block_list_offset = blocks.size();
    entry.block_count = 0;

    if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
        entry.rdev_major = major(st.st_rdev);
        entry.rdev_minor = minor(st.st_rdev);
    }

    if (!S_ISLNK(st.st_mode)) { // listxattr/setxattr follow symlinks
        std::vector<std::uint8_t> blob;
        util::read_xattrs(full, blob);
        if (!blob.empty()) entry.xattr_id = add_blob(blob);
        blob.clear();
        util::read_acl(full, blob);
        if (!blob.empty()) {
            blob.push_back('\0');
            entry.acl_id = add_blob(blob);
        }
    }

    total_uncompressed += entry.uncompressed_size;
    entries.push_back(entry);
}

io::pipeline* writer::impl::get_pipeline() {
    if (!pipe) {
        // Pin the section codec (same contract as add_file_impl). "none"
        // stores raw blocks: no codec instance, compressor id 0.
        if (!comp && section_compressor_id == format::COMPRESSOR_NONE &&
            opts.compressor != "none") {
            comp = compress::make_compressor(opts.compressor, opts.compression_level);
            section_compressor_id = comp->id();
        }
        io::pipeline::options po;
        po.files_in_flight = static_cast<int>(std::max<std::size_t>(opts.pipeline_depth, 1));
        po.compress_threads = std::max(1, opts.threads);
        // block_permits / per_file_cap keep their defaults; the constructor
        // scales the pool to the per-file liveness bound.
        pipe = std::make_unique<io::pipeline>(
            po, opts.compressor, opts.compression_level,
            /*use_compression=*/ opts.compressor != "none", opts.dedup);
    }
    return pipe.get();
}

// Drain one submitted file in walk order: stream its blocks (dedup-checked)
// straight to disk, then complete the file_entry from the terminal record.
void writer::impl::consume_pipelined_file(const std::string& rel, std::uint64_t seq)
{
    format::file_entry entry{};
    entry.path_id = add_string(rel);
    entry.block_list_offset = blocks.size();

    std::size_t i = 0;
    io::pipeline_block pb;
    while (pipe->next_block(seq, i, pb)) {
        bool dup = false;
        if (opts.dedup) {
            auto hit = dedup_lookup.find(pb.content_id);
            if (hit != dedup_lookup.end()) {
                // Duplicate: self-describing copy of the owner's fields.
                const auto& owner = blocks[dedup_index[hit->second].block_index];
                format::block_entry be{};
                be.offset            = owner.offset;
                be.compressed_size   = owner.compressed_size;
                be.uncompressed_size = pb.uncompressed_size;
                be.content_id        = pb.content_id;
                be.crc32c            = owner.crc32c;
                be.compressor_id     = owner.compressor_id;
                dedup_index[hit->second].ref_count++;
                blocks.push_back(be);
                dup = true;
            }
        }
        if (!dup) {
            format::block_entry be{};
            be.offset = DATA_OFFSET + data_written; // section-relative
            write_at(section_start + be.offset, pb.compressed.data(), pb.compressed.size());
            data_written += pb.compressed.size();

            be.compressed_size   = static_cast<std::uint32_t>(pb.compressed.size());
            be.uncompressed_size = pb.uncompressed_size;
            be.content_id        = pb.content_id;
            be.crc32c            = pb.crc32c;
            be.compressor_id     = pb.compressor_id;
            blocks.push_back(be);

            if (opts.dedup) {
                dedup_lookup[pb.content_id] = dedup_index.size();
                dedup_index.push_back(
                    {pb.content_id, be.offset, blocks.size() - 1, 1});
            }
        }
        pipe->release_block(seq);
        pb = io::pipeline_block{};
        i++;
    }

    auto fin = pipe->wait_file_done(seq);
    if (fin.error) {
        SPDLOG_ERROR("pipeline read failed for {}: {}",
                     fin.path.c_str(), fin.error.message());
        throw std::system_error(fin.error, "pipeline read failed: " + fin.path.string());
    }

    entry.mode = static_cast<std::uint16_t>(fin.st.st_mode);
    entry.uid  = fin.st.st_uid;
    entry.gid  = fin.st.st_gid;
    entry.mtime_sec  = static_cast<std::uint64_t>(fin.st.st_mtim.tv_sec);
    entry.mtime_nsec = static_cast<std::uint32_t>(fin.st.st_mtim.tv_nsec);
    entry.atime_sec  = static_cast<std::uint64_t>(fin.st.st_atim.tv_sec);
    entry.atime_nsec = static_cast<std::uint32_t>(fin.st.st_atim.tv_nsec);
    entry.nlink      = fin.st.st_nlink;
    entry.uncompressed_size = static_cast<std::uint64_t>(fin.st.st_size);
    entry.checksum = fin.file_crc32c;
    entry.block_count = blocks.size() - entry.block_list_offset;

    // Xattr / ACL blobs live in the string table (xattr_id/acl_id = offset).
    if (!fin.xattr_blob.empty()) entry.xattr_id = add_blob(fin.xattr_blob);
    if (!fin.acl_blob.empty()) {
        fin.acl_blob.push_back('\0'); // ACL text is NUL-terminated
        entry.acl_id = add_blob(fin.acl_blob);
    }

    total_uncompressed += entry.uncompressed_size;
    entries.push_back(entry);
}

// Write file_entry[], block_entry[], string table, dedup index; compute the
// metadata CRC; patch the reserved toc_header. Data blocks already streamed.
void writer::impl::finalize_section()
{
    if (!section_open) return;

    std::vector<std::uint8_t> file_tbl, block_tbl, dedup_tbl;
    file_tbl.reserve(entries.size() * format::FILE_ENTRY_SIZE);
    block_tbl.reserve(blocks.size() * format::BLOCK_ENTRY_SIZE);
    for (const auto& e : entries) {
        auto b = serialize_file_entry(e);
        file_tbl.insert(file_tbl.end(), b.begin(), b.end());
    }
    for (const auto& e : blocks) {
        auto b = serialize_block_entry(e);
        block_tbl.insert(block_tbl.end(), b.begin(), b.end());
    }
    if (opts.dedup) {
        // On-disk index sorted by content_hash (memcmp) for binary search.
        auto sorted = dedup_index;
        std::sort(sorted.begin(), sorted.end(),
                  [](const dedup_node& a, const dedup_node& b) {
                      return std::memcmp(a.hash.data(), b.hash.data(), 16) < 0;
                  });
        for (const auto& n : sorted) {
            format::dedup_entry de{};
            de.content_hash    = n.hash;
            de.physical_offset = n.physical_offset;
            de.reference_count = n.ref_count;
            auto b = serialize_dedup_entry(de);
            dedup_tbl.insert(dedup_tbl.end(), b.begin(), b.end());
        }
    }

    const std::uint64_t section_size_bytes =
        DATA_OFFSET + data_written + file_tbl.size() + block_tbl.size() +
        string_table.size() + dedup_tbl.size();

    format::toc_header h{};
    h.magic               = TOC_MAGIC_U32;
    h.entry_count         = entries.size();
    h.block_count         = blocks.size();
    h.file_table_offset   = DATA_OFFSET + data_written;
    h.block_table_offset  = h.file_table_offset + file_tbl.size();
    h.data_offset         = DATA_OFFSET;
    h.data_size           = data_written;
    h.string_table_offset = h.block_table_offset + block_tbl.size();
    h.string_table_size   = string_table.size();
    h.dedup_index_offset  = dedup_tbl.empty() ? 0 : h.string_table_offset + string_table.size();
    h.dedup_index_size    = dedup_tbl.size();
    h.compressor_id       = section_compressor_id;
    h.compression_level   = static_cast<std::int16_t>(opts.compression_level);
    h.section_flags       = opts.dedup ? format::SECTION_FLAG_DEDUP : 0;
    h.metadata_crc        = 0; // covered field — zeroed for its own CRC

    // Durability ordering (PLAN Phase 1 item 13): data → fsync → TOC.
    sync_now();

    // Metadata CRC32C: header (field zeroed) + file/block tables + strings +
    // dedup index. Data blocks NOT covered — keeps list() cheap (frozen).
    std::uint32_t crc = 0;
    {
        auto hdr_bytes = serialize_toc_header(h);
        crc = crc32c::Extend(crc, hdr_bytes.data(), hdr_bytes.size());
    }
    crc = crc32c::Extend(crc, file_tbl.data(), file_tbl.size());
    crc = crc32c::Extend(crc, block_tbl.data(), block_tbl.size());
    crc = crc32c::Extend(crc,
        reinterpret_cast<const std::uint8_t*>(string_table.data()), string_table.size());
    crc = crc32c::Extend(crc, dedup_tbl.data(), dedup_tbl.size());
    h.metadata_crc = crc;

    // Patch the reserved toc_header, then append the tables.
    auto hdr_bytes = serialize_toc_header(h);
    write_at(section_start, hdr_bytes.data(), hdr_bytes.size());
    write_at(section_start + h.file_table_offset, file_tbl.data(), file_tbl.size());
    write_at(section_start + h.block_table_offset, block_tbl.data(), block_tbl.size());
    write_at(section_start + h.string_table_offset, string_table.data(), string_table.size());
    if (h.dedup_index_offset)
        write_at(section_start + h.dedup_index_offset, dedup_tbl.data(), dedup_tbl.size());

    file_end = section_start + section_size_bytes;
    toc_refs.push_back({section_start, section_size_bytes, h.metadata_crc, 0});
    total_compressed += data_written;
    section_open = false;
}

void writer::impl::write_footer()
{
    // TOC table: array of toc_section_ref records.
    std::vector<std::uint8_t> table;
    table.reserve(toc_refs.size() * format::TOC_SECTION_REF_SIZE);
    for (const auto& r : toc_refs) {
        auto b = serialize_toc_ref(r);
        table.insert(table.end(), b.begin(), b.end());
    }
    const std::uint64_t table_off = file_end;
    write_at(table_off, table.data(), table.size());
    file_end += table.size();  // footer goes immediately after the table
    const std::uint32_t table_crc = crc32c::Crc32c(table.data(), table.size());

    // Assemble the 256-byte footer per docs/format.md §Footer.
    std::vector<std::uint8_t> f;
    f.reserve(format::FOOTER_SIZE);
    put_raw(f, "AR01", 4);                                            // 0
    put_be<std::uint16_t>(f, 0x0002);                                 // 4  version 0.2
    put_be<std::uint16_t>(f, static_cast<std::uint16_t>(
        (opts.dedup ? format::FOOTER_FLAG_DEDUP : 0) | footer_flags_inherited)); // 6
    put_be<std::uint64_t>(f, toc_refs.size());                        // 8
    put_be<std::uint64_t>(f, table_off);                              // 16
    put_be<std::uint64_t>(f, total_uncompressed);                     // 24
    put_be<std::uint64_t>(f, total_compressed);                       // 32
    put_be<std::uint32_t>(f, table_crc);                              // 40
    put_be<std::uint32_t>(f, 0);                                      // 44  footer_crc (patched)
    const auto foot0 = f.size();
    put_zeros(f, 200);                                                // 48  reserved
    const std::size_t foot_crc_off = 44;
    // footer_crc covers [0,44) ∪ [48,248):
    std::uint32_t fcrc = crc32c::Extend(0, f.data(), foot_crc_off);
    fcrc = crc32c::Extend(fcrc, f.data() + 48, f.size() - 48);
    {
        auto be = format::htobe32_safe(fcrc);
        std::memcpy(f.data() + foot_crc_off, &be, 4);
    }
    (void)foot0;
    put_be<std::uint32_t>(f, 0);   // 248 signature_crc (patched next)
    put_be<std::uint32_t>(f, 0);   // 252 reserved
    if (f.size() != format::FOOTER_SIZE) std::abort();
    const std::uint32_t sig = crc32c::Crc32c(f.data(), 248);
    {
        auto be = format::htobe32_safe(sig);
        std::memcpy(f.data() + 248, &be, 4);
    }

    // Final durability step: TOC table + footer as one fsync-bounded write.
    sync_now();
    write_at(file_end, f.data(), f.size());
    file_end += format::FOOTER_SIZE;
    sync_now();
}

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

    // 16-byte archive header: "ARCH\x01\x00" + 10 zero bytes (advisory only —
    // the footer is authoritative, spec §Archive header).
    std::array<std::uint8_t, 16> hdr{};
    std::memcpy(hdr.data(), format::ARCH_MAGIC.data(), format::ARCH_MAGIC.size());
    d->write_at(0, hdr.data(), hdr.size());
    d->file_end = hdr.size();
}

writer::~writer() {
    if (d->arc_fd >= 0) ::close(d->arc_fd);
}

void writer::add_file(std::filesystem::path path) {
    if (d->finalized) throw std::runtime_error("writer already finalized");
    d->ensure_section();

    // NB: stat (not lstat) — add_file follows symlinks and archives their
    // target's content; add_tree stores symlinks as symlinks instead.
    struct stat st{};
    if (stat(path.c_str(), &st)) {
        int e = errno;
        SPDLOG_ERROR("stat failed for {}: {}", path.c_str(), strerror(e));
        throw std::system_error(e, std::generic_category(), "stat failed");
    }

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        int e = errno;
        SPDLOG_ERROR("open failed for {}: {}", path.c_str(), strerror(e));
        throw std::system_error(e, std::generic_category(), "open failed");
    }

    d->add_file_impl(path.filename(), path, static_cast<std::uint64_t>(st.st_size), fd);
}

void writer::add_file(std::filesystem::path path, std::string_view data) {
    if (d->finalized) throw std::runtime_error("writer already finalized");
    d->ensure_section();
    if (!d->comp) {
        d->comp = compress::make_compressor(d->opts.compressor,
                                            d->opts.compression_level);
        d->section_compressor_id = d->comp->id();
    }

    format::file_entry entry{};
    entry.mode = S_IFREG | 0644;
    entry.uncompressed_size = data.size();
    entry.path_id = d->add_string(path.string());
    entry.block_list_offset = d->blocks.size();

    std::size_t off = 0;
    std::uint32_t file_crc = 0;
    while (off < data.size()) {
        std::size_t bsize = std::min(format::ARCH_BLOCK_SIZE, data.size() - off);
        auto chunk = std::span(reinterpret_cast<const std::uint8_t*>(data.data() + off), bsize);
        file_crc = crc32c::Extend(file_crc, chunk.data(), chunk.size());

        format::digest128 blk_hash{};
        if (d->opts.dedup) {
            blk_hash = hash_block(chunk);
            auto hit = d->dedup_lookup.find(blk_hash);
            if (hit != d->dedup_lookup.end()) {
                const auto& owner = d->blocks[d->dedup_index[hit->second].block_index];
                format::block_entry be{};
                be.offset            = owner.offset;
                be.compressed_size   = owner.compressed_size;
                be.uncompressed_size = static_cast<std::uint32_t>(bsize);
                be.content_id        = blk_hash;
                be.crc32c            = owner.crc32c;
                be.compressor_id     = owner.compressor_id;
                d->dedup_index[hit->second].ref_count++;
                d->blocks.push_back(be);
                off += bsize;
                continue;
            }
        }

        auto cb = d->comp->compress(chunk);

        format::block_entry be{};
        be.offset = impl::DATA_OFFSET + d->data_written;
        d->write_at(d->section_start + be.offset, cb.data.data(), cb.data.size());
        d->data_written += cb.data.size();

        be.compressed_size   = static_cast<std::uint32_t>(cb.data.size());
        be.uncompressed_size = static_cast<std::uint32_t>(bsize);
        be.content_id        = blk_hash;
        be.crc32c            = cb.crc32c;
        be.compressor_id     = cb.compressor_id;
        d->blocks.push_back(be);

        if (d->opts.dedup) {
            d->dedup_lookup[blk_hash] = d->dedup_index.size();
            d->dedup_index.push_back(
                {blk_hash, be.offset, d->blocks.size() - 1, 1});
        }
        off += bsize;
    }

    entry.checksum = file_crc;
    entry.block_count = d->blocks.size() - entry.block_list_offset;
    d->total_uncompressed += entry.uncompressed_size;
    d->entries.push_back(entry);
}

void writer::add_tree(const std::filesystem::path& root) {
    if (d->finalized) throw std::runtime_error("writer already finalized");

    struct stat rst{};
    if (::stat(root.c_str(), &rst) || !S_ISDIR(rst.st_mode)) {
        throw std::system_error(ENOTDIR, std::generic_category(),
                                "add_tree requires a directory: " + root.string());
    }
    d->ensure_section();

    namespace fs = std::filesystem;
    // Deterministic depth-first walk (names sorted per directory). Archive
    // paths are relative to `root` (frozen decision); the root itself is not
    // stored as an entry. The walk only collects jobs; consumption happens in
    // the exact same order afterwards, so output is scheduling-independent.
    struct tree_item {
        fs::path abs;
        std::string rel;
        struct stat st{};
        std::string text;       // symlink target / hardlink owner rel-path
        enum kind_t { kMeta, kSymlink, kHardlinkDup, kFile } kind = kMeta;
        std::uint64_t seq = ~std::uint64_t{0};  // pipeline sequence (kFile)
        bool done = false;                      // consumed (or reaped after error)
    };
    std::vector<tree_item> items;

    std::vector<std::pair<fs::path /*abs*/, std::string /*rel*/>> stack;
    std::vector<std::pair<std::string, fs::directory_entry>> kids;

    auto walk_dir = [&](const fs::path& abs, const std::string& rel) {
        kids.clear();
        for (const auto& e : fs::directory_iterator(abs))
            kids.emplace_back(rel.empty() ? e.path().filename().string()
                                          : rel + "/" + e.path().filename().string(),
                              e);
        std::sort(kids.begin(), kids.end(),
                  [](auto& a, auto& b) { return a.first < b.first; });
        // Push in reverse so the LIFO stack pops them in sorted order — the
        // first path of a shared inode (hardlink owner) is alphabetical-first.
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            stack.emplace_back(fs::absolute(it->second.path()), it->first);
    };

    walk_dir(fs::absolute(root), std::string());

    while (!stack.empty()) {
        auto [abs, rel] = stack.back();
        stack.pop_back();

        tree_item it;
        it.abs = std::move(abs);
        it.rel = std::move(rel);
        if (::lstat(it.abs.c_str(), &it.st)) {
            int e2 = errno;
            SPDLOG_ERROR("lstat failed for {}: {}", it.abs.c_str(), strerror(e2));
            throw std::system_error(e2, std::generic_category(), "lstat failed: " + it.abs.string());
        }

        if (S_ISDIR(it.st.st_mode)) {
            it.kind = tree_item::kMeta;
            walk_dir(it.abs, it.rel);
        } else if (S_ISLNK(it.st.st_mode)) {
            it.kind = tree_item::kSymlink;
            it.text = fs::read_symlink(it.abs).string();
        } else if (S_ISREG(it.st.st_mode) && it.st.st_nlink > 1) {
            // Hardlink: first path storing data owns the inode; duplicates
            // reference it via link_target_id (spec §File Entry).
            auto key = std::make_pair(static_cast<std::uint64_t>(it.st.st_dev),
                                      static_cast<std::uint64_t>(it.st.st_ino));
            if (auto found = d->inode_paths.find(key); found != d->inode_paths.end()) {
                it.kind = tree_item::kHardlinkDup;
                it.text = found->second;
            } else {
                d->inode_paths[key] = it.rel;
                it.kind = tree_item::kFile;
            }
        } else if (S_ISREG(it.st.st_mode)) {
            it.kind = tree_item::kFile;
        } else {
            it.kind = tree_item::kMeta; // blk/chr/fifo/sock: metadata-only
        }
        items.push_back(std::move(it));
    }

    // ---- consumption, strictly in walk order ----
    auto consume_meta = [&](tree_item& it) {
        switch (it.kind) {
        case tree_item::kSymlink:
            d->add_metadata_entry(it.rel, it.abs, it.st,
                                  /*symlink_target_id=*/d->add_string(it.text),
                                  /*link_target_id=*/0,
                                  /*uncompressed_size_override=*/it.text.size());
            break;
        case tree_item::kHardlinkDup:
            d->add_metadata_entry(it.rel, it.abs, it.st, 0,
                                  /*link_target_id=*/d->add_string(it.text),
                                  /*override 0 → falls back to st_size (informational;
                                    no block list — extract re-links to the owner)*/ 0);
            break;
        default:
            d->add_metadata_entry(it.rel, it.abs, it.st);
        }
    };

    if (d->opts.threads <= 0) {
        // Serial fallback (also the reference for determinism tests).
        for (auto& it : items) {
            if (it.kind != tree_item::kFile) { consume_meta(it); continue; }
            int fd = ::open(it.abs.c_str(), O_RDONLY | O_NOFOLLOW);
            if (fd < 0) {
                int e2 = errno;
                throw std::system_error(e2, std::generic_category(), "open failed: " + it.abs.string());
            }
            d->add_file_impl(it.rel, it.abs,
                             static_cast<std::uint64_t>(it.st.st_size), fd);
        }
        return;
    }

    // Phase 3 pipeline: keep up to `depth` files in flight. Files are
    // submitted in walk order and consumed in walk order; the window keeps
    // I/O workers busy (network-FS latency hidden across files) while the
    // writer streams blocks to disk in the exact serial-path order.
    auto* pipe = d->get_pipeline();
    const std::size_t depth = std::max<std::size_t>(d->opts.pipeline_depth, 1);
    std::size_t next_consume = 0;

    auto consume_one = [&](tree_item& it) {
        if (it.kind == tree_item::kFile) d->consume_pipelined_file(it.rel, it.seq);
        else consume_meta(it);
        it.done = true;
    };

    auto reap_rest = [&] {
        // Error path: retire every submitted-but-undrained file so the
        // writer stays usable (in-flight slots and permits freed).
        for (auto& it : items)
            if (it.kind == tree_item::kFile && !it.done) {
                try { pipe->wait_file_done(it.seq); } catch (...) {}
                it.done = true;
            }
    };

    try {
        for (std::size_t i = 0; i < items.size(); ++i) {
            auto& it = items[i];
            if (it.kind != tree_item::kFile) continue;
            // Free an in-flight slot if the window is full (submit-order drain;
            // every item before i was already submitted, so this always ends).
            while (pipe->in_flight() >= depth)
                consume_one(items[next_consume++]);
            it.seq = pipe->submit(it.abs,
                                  static_cast<std::uint64_t>(it.st.st_size),
                                  /*follow_symlinks=*/false);
        }
        while (next_consume < items.size())
            consume_one(items[next_consume++]);
    } catch (...) {
        reap_rest();
        throw;
    }
}

void writer::finalize() {
    if (d->finalized) return;
    d->ensure_section();    // an empty archive still gets one (empty) section
    d->finalize_section();
    d->write_footer();
    d->finalized = true;
    ::close(d->arc_fd);
    d->arc_fd = -1;
}

// ---- writer::append (Phase 4): open existing archive, keep its sections ----
//
// Validates the same chain the reader uses (footer magic + signature CRC +
// self-check CRC + version + TOC-table CRC), then re-verifies every existing
// section's metadata CRC so a damaged tail is treated as a clean truncation
// point (durable-append contract, docs/format.md §CRC scheme). The old TOC
// table + footer are discarded (ftruncate): the next finalize()/added section
// rewrites them at the kept-region end.

namespace {

std::vector<std::uint8_t> pread_exact(int fd, std::uint64_t off, std::size_t n,
                                      const char* what) {
    std::vector<std::uint8_t> b(n);
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::pread(fd, b.data() + got, n - got, static_cast<off_t>(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            throw std::system_error(e, std::generic_category(), what);
        }
        if (r == 0)
            throw std::runtime_error(std::string("archive truncated reading ") + what);
        got += static_cast<std::size_t>(r);
    }
    return b;
}

} // namespace

writer::writer(std::filesystem::path path, options opts, append_tag)
    : d(std::make_unique<impl>(std::move(opts), std::move(path)))
{
    d->arc_fd = ::open(d->arc_path.c_str(), O_RDWR);
    if (d->arc_fd < 0) {
        int e = errno;
        SPDLOG_ERROR("cannot open {} for append: {}", d->arc_path.c_str(), strerror(e));
        throw std::system_error(e, std::generic_category(),
                                "cannot open " + d->arc_path.string());
    }

    struct stat st{};
    if (::fstat(d->arc_fd, &st) != 0)
        throw std::system_error(errno, std::generic_category(), "fstat failed");
    const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);
    const std::string& p = d->arc_path.string();
    if (file_size < format::ARCHIVE_HEADER_SIZE + format::FOOTER_SIZE)
        throw std::runtime_error("file too small to be a finalized archive: " + p);

    // Footer: authoritative validity check (mirrors src/archive/reader.cpp).
    auto fbytes = pread_exact(d->arc_fd, file_size - format::FOOTER_SIZE,
                              format::FOOTER_SIZE, "footer");
    format::footer footer = detail::parse_footer(fbytes.data());
    if (std::memcmp(footer.magic.data(), format::FOOTER_MAGIC.data(), 4) != 0)
        throw std::runtime_error("bad footer magic — not an archive or corrupt: " + p);
    if (crc32c::Crc32c(fbytes.data(), 248) != footer.signature_crc)
        throw std::runtime_error("footer signature CRC mismatch in " + p);
    {
        std::uint32_t fcrc = crc32c::Extend(0, fbytes.data(), 44);
        fcrc = crc32c::Extend(fcrc, fbytes.data() + 48, 200);
        if (fcrc != footer.footer_crc)
            throw std::runtime_error("footer self-check CRC mismatch in " + p);
    }
    if (footer.version > 0x0002)
        throw std::runtime_error("unsupported archive format version " +
                                 std::to_string(footer.version));

    const std::uint64_t table_off = footer.toc_table_offset;
    if (table_off < format::ARCHIVE_HEADER_SIZE ||
        footer.toc_count >
            (file_size - format::FOOTER_SIZE - table_off) / format::TOC_SECTION_REF_SIZE)
        throw std::runtime_error("bogus TOC table in " + p);
    auto tbytes = pread_exact(d->arc_fd, table_off,
                              static_cast<std::size_t>(footer.toc_count * format::TOC_SECTION_REF_SIZE),
                              "TOC table");
    if (crc32c::Crc32c(tbytes.data(), tbytes.size()) != footer.toc_table_crc)
        throw std::runtime_error("TOC table CRC mismatch in " + p);

    std::vector<format::toc_section_ref> refs;
    refs.reserve(footer.toc_count);
    for (std::size_t i = 0; i < footer.toc_count; ++i)
        refs.push_back(detail::parse_toc_ref(tbytes.data() +
                                             i * format::TOC_SECTION_REF_SIZE));

    // Re-verify each section header + metadata CRC. First failure = clean
    // truncation point: drop that section and everything after it.
    std::vector<format::toc_section_ref> kept;
    std::uint64_t uncomp = 0, comp_bytes = 0;
    std::uint64_t truncate_at = table_off; // all sections kept (default)
    for (const auto& r : refs) {
        bool ok = r.offset >= format::ARCHIVE_HEADER_SIZE &&
                  r.size >= format::TOC_HEADER_SIZE &&
                  r.offset <= table_off && r.size <= table_off - r.offset;
        if (ok) {
            auto hbytes = pread_exact(d->arc_fd, r.offset, format::TOC_HEADER_SIZE,
                                      "TOC header");
            auto h = detail::parse_toc_header(hbytes.data());
            constexpr std::uint32_t toc_magic =
                (static_cast<std::uint32_t>('T') << 24) |
                (static_cast<std::uint32_t>('O') << 16) |
                (static_cast<std::uint32_t>('C') << 8) |
                static_cast<std::uint32_t>('1');
            const std::uint64_t sz = r.size;
            ok = h.magic == toc_magic &&
                 h.data_offset >= format::TOC_HEADER_SIZE &&
                 h.data_size <= sz - h.data_offset &&
                 h.file_table_offset >= h.data_offset + h.data_size &&
                 h.entry_count <= (sz - h.file_table_offset) / format::FILE_ENTRY_SIZE &&
                 h.block_table_offset >=
                     h.file_table_offset + h.entry_count * format::FILE_ENTRY_SIZE &&
                 h.block_count <= (sz - h.block_table_offset) / format::BLOCK_ENTRY_SIZE;
            if (ok) {
                auto tail = pread_exact(d->arc_fd, r.offset + h.file_table_offset,
                                        static_cast<std::size_t>(sz - h.file_table_offset),
                                        "TOC metadata");
                std::array<std::uint8_t, format::TOC_HEADER_SIZE> hb{};
                std::memcpy(hb.data(), hbytes.data(), hb.size());
                std::fill(hb.begin() + 92, hb.begin() + 96, std::uint8_t{0});
                std::uint32_t crc = crc32c::Crc32c(hb.data(), hb.size());
                crc = crc32c::Extend(crc, tail.data(), tail.size());
                ok = crc == h.metadata_crc && crc == r.metadata_crc;
                if (ok) {
                    // Recompute archive totals from the kept region. tail
                    // starts exactly at the file_entry table.
                    for (std::uint64_t i = 0; i < h.entry_count; ++i) {
                        auto e = detail::parse_file_entry(
                            tail.data() + i * format::FILE_ENTRY_SIZE);
                        uncomp += e.uncompressed_size;
                    }
                    comp_bytes += h.data_size;
                }
            }
        }
        if (!ok) {
            SPDLOG_WARN("{}: TOC section at {} failed validation — truncation "
                        "point, dropping it and later sections", p, r.offset);
            truncate_at = (r.offset >= format::ARCHIVE_HEADER_SIZE && r.offset <= table_off)
                              ? r.offset : table_off;
            break;
        }
        kept.push_back(r);
    }
    d->toc_refs = std::move(kept);

    // When everything validated, trust the footer's totals (authoritative,
    // cheap); otherwise use the recomputed kept-region sums.
    if (d->toc_refs.size() == refs.size()) {
        d->total_uncompressed = footer.total_uncompressed_size;
        d->total_compressed = footer.total_compressed_size;
    } else {
        d->total_uncompressed = uncomp;
        d->total_compressed = comp_bytes;
    }
    d->footer_flags_inherited =
        footer.flags.dedup() ? format::FOOTER_FLAG_DEDUP : 0;

    // Discard the old TOC table + footer; the next write starts at EOF again.
    if (::ftruncate(d->arc_fd, static_cast<off_t>(truncate_at)) != 0)
        throw std::system_error(errno, std::generic_category(),
                                "ftruncate failed on " + p);
    d->file_end = truncate_at;
}

writer writer::append(std::filesystem::path path, options opts) {
    return writer(std::move(path), std::move(opts), append_tag{});
}

} // namespace arch
