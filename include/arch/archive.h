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
        std::string compressor = "zstd";   // compressor name
        int threads = 4;                  // threads for parallel compression
        bool dedup = false;               // enable content-aware dedup
        std::size_t prefetch_window = 16;  // small-file prefetch batch size
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

    /// Finalize: write TOC section + footer.
    void finalize();

    /// Append an already-finalized section to an existing archive.
    void append(std::filesystem::path path);

private:
    struct impl;
    std::unique_ptr<impl> d;
};

/// High-level archive reader / extractor.
class reader {
public:
    explicit reader(std::filesystem::path path);
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
