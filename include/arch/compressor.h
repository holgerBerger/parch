#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <span>

namespace arch::compress {

/// Opaque result of a single-block compression operation.
struct compressed_block {
    std::vector<std::uint8_t> data;
    std::uint32_t crc32c{};
    std::uint8_t compressor_id{};
};

/// Base class for pluggable compressors.
/// Every compressor works on exactly one block (≤ 4 MiB).
/// Multi-threading is handled by the caller (Writer) via a work queue.
class compressor {
public:
    virtual ~compressor() = default;

    /// ID returned to the caller so the block_entry knows what codec was used.
    virtual std::uint8_t id() const = 0;
    /// Human-readable name.
    virtual std::string_view name() const = 0;

    /// Compress a single block.
    /// The output buffer is allocated by the compressor.
    virtual compressed_block compress(std::span<const std::uint8_t> input) = 0;

    /// Decompress a single block. Returns empty vector on failure.
    /// `level` only matters for codecs that need it to init the decompressor
    /// context; the block header carries the actual parameters.
    virtual std::vector<std::uint8_t> decompress(
        std::span<const std::uint8_t> input, std::uint32_t expected_uncompressed) = 0;

    /// Maximum memory usage hint (per-thread). Used to size decompression buffers.
    virtual std::size_t mem_usage_hint() const { return 0; }
};

/// Factory: build a compressor by name string at the given level.
/// Throws std::runtime_error for unknown names.
std::unique_ptr<compressor> make_compressor(std::string_view name, int level);

/// Factory for readers: build a compressor by its on-disk `compressor_id`
/// (docs/format.md §Compressor Registry). Probes registered factories once
/// (caching name→id) and instantiates the match at `level` (level only
/// affects compression; decompression ignores it).
/// Throws std::runtime_error if no registered codec carries that id.
std::unique_ptr<compressor> make_compressor_by_id(std::uint8_t id, int level);

/// Register a compressor class. Call once at program init.
/// Usage: REGISTER_COMPRESSOR("zstd", compress::zstd_impl);
#define REGISTER_COMPRESSOR(name, T)                                                    \
    namespace {                                                                         \
        struct __reg_##T {                                                              \
            __reg_##T() { compress::register_compressor(                                \
                name, [](int level){ return std::make_unique<T>(level); }); }           \
        };                                                                              \
        __reg_##T __inst_##T;                                                           \
    }

void register_compressor(std::string_view name,
                         std::function<std::unique_ptr<compressor>(int level)> factory);

} // namespace arch::compress
