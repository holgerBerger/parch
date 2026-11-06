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
    virtual std::vector<std::uint8_t> decompress(
        std::span<const std::uint8_t> input, std::uint32_t expected_uncompressed) = 0;

    /// Maximum memory usage hint (per-thread). Used to size decompression buffers.
    virtual std::size_t mem_usage_hint() const { return 0; }
};

/// Factory: build a compressor by name string.
std::unique_ptr<compressor> make_compressor(std::string_view name);

/// Register a compressor class. Call once at program init.
/// Usage: REGISTER_COMPRESSOR("zstd", compress::zstd_impl);
#define REGISTER_COMPRESSOR(name, T)                                                    \
    namespace {                                                                         \
        struct __reg_##T {                                                              \
            __reg_##T() { compress::register_compressor(name, [](){ return std::make_unique<T>(); }); } \
        };                                                                              \
        __reg_##T __inst_##T;                                                           \
    }

void register_compressor(std::string_view name,
                         std::function<std::unique_ptr<compressor>()> factory);

} // namespace arch::compress
