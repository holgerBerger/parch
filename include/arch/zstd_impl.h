#pragma once

#include <vector>
#include <string_view>
#include <memory>

namespace arch::compress {
class compressor;
}

namespace arch::compress {

/// Zstandard compressor implementation.
/// Configurable thread count (internal to zstd's dictionary/streaming mode).
class zstd_impl : public compressor {
public:
    explicit zstd_impl(int threads = 0); // 0 = auto

    std::uint8_t id() const override;
    std::string_view name() const override;
    compressed_block compress(std::span<const std::uint8_t> input) override;
    std::vector<std::uint8_t> decompress(
        std::span<const std::uint8_t> input, std::uint32_t expected_uncompressed) override;
    std::size_t mem_usage_hint() const override;

private:
    int m_threads;
    std::unique_ptr<void, void (*)(void*)> m_cctx; // ZSTD_CCtx*
};

} // namespace arch::compress
