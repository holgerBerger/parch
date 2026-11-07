#pragma once

#include "arch/format.h"
#include <vector>
#include <string>
#include <string_view>
#include <optional>
#include <memory>
#include <filesystem>

namespace arch {

/// High-level archive writer.
class writer {
public:
    /// Configuration for the writer.
    struct options {
        options() {}  // user-provided: GCC 15 rejects `opts = {}` default
                      // arguments for nested classes whose only ctor is the
                      // implicit one with NSDMIs ("required before the end of
                      // its enclosing class").
        std::string compressor = "zstd";   // compressor name (factory registry)
        int compression_level = 3;         // codec level; pinned into the TOC section header
        int threads = 4;                   // block-compression pool size
        std::size_t pipeline_depth = 32;   // files in flight in the I/O pipeline (Phase 3)
        bool dedup = false;                // enable within-section content dedup
        bool durable = true;               // fsync ordering on finalize/append
        bool sparse = true;                // archive holes as sparse blocks (v0.3)
        bool xattrs = true;                // probe + capture extended attributes
                                           // (false skips the listxattr/getxattr
                                           // round-trips — big on network FSes)
        bool acls = true;                  // probe + capture POSIX ACLs
                                           // (false skips the two acl_get RPCs/file)
        bool profile = false;              // collect per-stage create timings;
                                           // report emitted on stderr by finalize(),
                                           // see profile_report()
    };

    explicit writer(std::filesystem::path path, options opts = {});
    ~writer();

    // Non-copyable
    writer(const writer&) = delete;
    writer& operator=(const writer&) = delete;

    /// Add a file from the filesystem.
    /// Reads metadata (mode, uid, gid, times, xattrs, ACLs) automatically.
    /// Large files are split into blocks, each compressed in parallel.
    void add_file(std::filesystem::path path);

    /// Add a file with explicit data (in-memory source).
    void add_file(std::filesystem::path path, std::string_view data);

    /// Recursively add a directory tree as first-class entries: files,
    /// directories, symlinks (target stored via symlink_target_id), hardlinks
    /// (duplicates reference the first path via link_target_id), and device
    /// nodes (metadata-only with rdev). Names are stored relative to `root`;
    /// the root itself is not an entry. Walk order is deterministic
    /// (alphabetical per directory).
    ///
    /// With a non-empty `store_prefix` (tar-style storage of the top level):
    /// the root itself becomes a directory entry named `store_prefix` and
    /// every descendant is stored under `store_prefix/...`. The prefix must
    /// be a plain relative path without "." / ".." components (throws
    /// std::runtime_error otherwise); intermediate directories of a
    /// multi-component prefix need not exist as entries — extraction creates
    /// them.
    void add_tree(const std::filesystem::path& root, std::string store_prefix = {});

    /// Finalize: write TOC section + footer.
    void finalize();

    /// With options.profile: a human-readable per-stage timing breakdown of
    /// everything added so far (also printed to stderr by finalize()). Empty
    /// string when profiling is off. Cumulative across add_file/add_tree/
    /// finalize; covers both the pipelined and the serial fallback paths.
    std::string profile_report() const;

    /// Open an existing finalized archive for appending: validates the
    /// footer + TOC table, keeps the existing sections, and truncates the
    /// old TOC table + footer so the next added section (or `finalize()`'s
    /// empty one) starts at the old footer position. Dedup never crosses
    /// sections (frozen). Throws `std::system_error` / `std::runtime_error`
    /// on a missing or invalid archive.
    static writer append(std::filesystem::path path, options opts = {});

private:
    struct impl;
    std::unique_ptr<impl> d;

    // Used by the static append() factory: opens the existing archive
    // O_RDWR (no truncation) after validating footer + TOC table.
    struct append_tag {};
    writer(std::filesystem::path path, options opts, append_tag);
};

/// High-level archive reader / extractor.
class reader {
public:
    /// Configuration for the reader.
    struct options {
        options() {}  // explicit default ctor (GCC 15 nested-class quirk,
                      // same reason as writer::options)
        int threads = 0;     // CRC+decompress concurrency (permits); 0 → hardware concurrency
        int depth = 32;      // extract_all/verify_all workers: files in flight.
                             // The latency knob (Phase 10, docs/extract-pipeline.md).
        bool profile = false; // collect per-stage timings; report emitted on
                              // stderr after extract_all/verify_all, see profile_report()
    };

    explicit reader(std::filesystem::path path, options opts = {});
    ~reader();

    reader(const reader&) = delete;
    reader& operator=(const reader&) = delete;

    /// List all entries across all TOC sections.
    struct listing_entry {
        std::filesystem::path path;
        format::file_type type;
        std::uint64_t size;
        std::uint16_t mode;
        std::uint32_t uid;
        std::uint32_t gid;
        std::uint64_t mtime;
        std::string link_target;  // symlink target or hardlink canonical path; empty if neither
    };
    std::vector<listing_entry> list() const;

    /// Extract a single entry to disk.
    void extract(const std::filesystem::path& arc_path,
                 std::filesystem::path dest) const;

    /// Extract all entries to a base directory.
    void extract_all(std::filesystem::path dest) const;

    /// Verify archive integrity without writing any output: reads and
    /// CRC-checks every block of every regular-file and hardlink entry
    /// (decompression included), and checks each file checksum. Returns one
    /// error string per failing entry — empty vector means the data is
    /// intact. Metadata-level validity is guaranteed by the constructor.
    std::vector<std::string> verify_all() const;

    /// With options.profile: a human-readable per-stage timing breakdown
    /// with tuning hints (also printed to stderr at the end of
    /// extract_all/verify_all). Empty string when profiling is off.
    std::string profile_report() const;

    /// Get extended attributes for an entry.
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>>
    get_xattrs(const std::filesystem::path& arc_path) const;

    /// Get ACL text for an entry.
    std::string get_acl(const std::filesystem::path& arc_path) const;

private:
    struct impl;
    std::unique_ptr<impl> d;
};

} // namespace arch
