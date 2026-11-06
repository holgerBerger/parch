#include "arch/zstd_impl.h"
#include "arch/format.h"
#include <zstd.h>
#include <crc32c/crc32c.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <cstring>

namespace arch::compress {

zstd_impl::zstd_impl(int level)
    : m_level(level)
    , m_cctx(ZSTD_createCCtx(), [](ZSTD_CCtx_s* c) { ZSTD_freeCCtx(c); })
{
    if (!m_cctx) {
        throw std::runtime_error("ZSTD_createCCtx failed");
    }
    ZSTD_CCtx_setParameter(m_cctx.get(), ZSTD_c_compressionLevel, level);
    // The block-compression pool owns parallelism (PLAN.md Phase 3): pin zstd
    // to a single worker so `threads` is not double-counted. The real param
    // name is ZSTD_c_nbWorkers (there is no ZSTD_c_nbThreads).
    ZSTD_CCtx_setParameter(m_cctx.get(), ZSTD_c_nbWorkers, 1);
}

std::uint8_t zstd_impl::id() const {
    return format::COMPRESSOR_ZSTD;
}

std::string_view zstd_impl::name() const {
    return "zstd";
}

compressed_block zstd_impl::compress(std::span<const std::uint8_t> input) {
    size_t max_out = ZSTD_COMPRESSBOUND(input.size());
    std::vector<std::uint8_t> out(max_out);

    size_t actual = ZSTD_compress2(
        reinterpret_cast<ZSTD_CCtx*>(m_cctx.get()),
        out.data(), max_out,
        input.data(), input.size());

    if (ZSTD_isError(actual)) {
        SPDLOG_ERROR("ZSTD_compress2 failed: {}", ZSTD_getErrorName(actual));
        throw std::runtime_error("ZSTD_compress2 failed: " + std::string(ZSTD_getErrorName(actual)));
    }

    out.resize(actual);
    std::uint32_t crc = crc32c::Crc32c(out.data(), out.size());

    compressed_block block;
    block.data = std::move(out);
    block.crc32c = crc;
    block.compressor_id = format::COMPRESSOR_ZSTD;
    return block;
}

std::vector<std::uint8_t> zstd_impl::decompress(
    std::span<const std::uint8_t> input, std::uint32_t expected_uncompressed)
{
    std::vector<std::uint8_t> out(expected_uncompressed);

    size_t actual = ZSTD_decompress(
        out.data(), out.size(),
        input.data(), input.size());

    if (ZSTD_isError(actual)) {
        return {}; // decompression failure
    }

    if (actual != expected_uncompressed) {
        return {}; // size mismatch
    }

    return out;
}

std::size_t zstd_impl::mem_usage_hint() const {
    // ZSTD_CStreamWorkspaceBound does not exist; the estimate* API requires
    // ZSTD_STATIC_LINKING_ONLY, so keep the hint to public-API bounds:
    return ZSTD_compressBound(format::ARCH_BLOCK_SIZE);
}

// Auto-register at static init.
REGISTER_COMPRESSOR("zstd", zstd_impl);

} // namespace arch::compress
