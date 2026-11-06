#include "arch/zstd_impl.h"
#include <zstd.h>
#include <crc32c/crc32c.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <cstring>

namespace arch::compress {

zstd_impl::zstd_impl(int threads)
    : m_threads(threads)
    , m_cctx(ZSTD_createCCtx(), ZSTD_freeCCtx)
{
    if (!m_cctx.get()) {
        throw std::runtime_error("ZSTD_createCCtx failed");
    }
    if (threads > 0) {
        ZSTD_CCtx_setParameter(reinterpret_cast<ZSTD_CCtx*>(m_cctx.get()),
                               ZSTD_c_nbThreads, threads);
    }
}

std::uint8_t zstd_impl::id() const {
    return COMPRESSOR_ZSTD;
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
    std::uint32_t crc = crc32c::Crc32c(crc32c::StringView(reinterpret_cast<const char*>(out.data()), out.size()));

    compressed_block block;
    block.data = std::move(out);
    block.crc32c = crc;
    block.compressor_id = COMPRESSOR_ZSTD;
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
    return ZSTD_CStreamWorkspaceBound(0) + ZSTD_DStreamWorkspaceBound(4 * 1024 * 1024);
}

// Auto-register at static init.
REGISTER_COMPRESSOR("zstd", zstd_impl);

} // namespace arch::compress
