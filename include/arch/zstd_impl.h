#pragma once

#include "arch/compressor.h"
#include <vector>
#include <string_view>
#include <memory>

struct ZSTD_CCtx_s;

namespace arch::compress {

/// Zstandard compressor implementation.
/// Runs single-threaded per block (nbWorkers=1); parallelism comes from the
/// writer's block-compression pool (PLAN.md Phase 3), not from zstd itself.
class zstd_impl : public compressor {
public:
    explicit zstd_impl(int level = 3);

    std::uint8_t id() const override;
    std::string_view name() const override;
    compressed_block compress(std::span<const std::uint8_t> input) override;
    std::vector<std::uint8_t> decompress(
        std::span<const std::uint8_t> input, std::uint32_t expected_uncompressed) override;
    std::size_t mem_usage_hint() const override;

private:
    int m_level;
    std::unique_ptr<ZSTD_CCtx_s, void (*)(ZSTD_CCtx_s*)> m_cctx;
};

} // namespace arch::compress
