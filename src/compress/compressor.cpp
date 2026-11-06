#include "arch/compressor.h"
#include "arch/zstd_impl.h"
#include <unordered_map>
#include <spdlog/spdlog.h>

namespace arch::compress {

namespace {
    std::mutex g_registry_mtx;
    std::unordered_map<std::string, std::function<std::unique_ptr<compressor>()>> g_registry;
}

void register_compressor(std::string_view name,
                         std::function<std::unique_ptr<compressor>()> factory)
{
    std::lock_guard lk(g_registry_mtx);
    g_registry[std::string(name)] = std::move(factory);
}

std::unique_ptr<compressor> make_compressor(std::string_view name)
{
    std::lock_guard lk(g_registry_mtx);
    auto it = g_registry.find(std::string(name));
    if (it == g_registry.end()) {
        SPDLOG_WARN("Unknown compressor requested: {}", name);
        throw std::runtime_error("Unknown compressor: " + std::string(name));
    }
    return it->second();
}

} // namespace arch::compress