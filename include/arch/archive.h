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
    void add_tree(const std::filesystem::path& root);

    /// Finalize: write TOC section + footer.
    void finalize();

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
        int threads = 0;   // extract_all worker pool size; 0 → hardware concurrency
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
