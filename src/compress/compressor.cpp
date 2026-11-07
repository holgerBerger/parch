#include "arch/compressor.h"
#include "arch/zstd_impl.h"
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <spdlog/spdlog.h>

namespace arch::compress {

namespace {
    std::mutex g_registry_mtx;
    std::unordered_map<std::string, std::function<std::unique_ptr<compressor>(int)>> g_registry;
}

void register_compressor(std::string_view name,
                         std::function<std::unique_ptr<compressor>(int level)> factory)
{
    std::lock_guard lk(g_registry_mtx);
    g_registry[std::string(name)] = std::move(factory);
}

std::unique_ptr<compressor> make_compressor(std::string_view name, int level)
{
    std::lock_guard lk(g_registry_mtx);
    auto it = g_registry.find(std::string(name));
    if (it == g_registry.end()) {
        SPDLOG_WARN("Unknown compressor requested: {}", name);
        throw std::runtime_error("Unknown compressor: " + std::string(name));
    }
    return it->second(level);
}

namespace {
    // id -> registered name, built by probing each factory once. Filled lazily
    // under g_registry_mtx; safe because registration is static-init-only.
    std::unordered_map<std::uint8_t, std::string> g_id_to_name;
    bool g_id_cache_ready = false;

    void rebuild_id_cache() {
        g_id_to_name.clear();
        for (auto& [name, factory] : g_registry) {
            try {
                auto probe = factory(1);
                if (probe) g_id_to_name[probe->id()] = name;
            } catch (...) { /* a broken factory must not poison the cache */ }
        }
        g_id_cache_ready = true;
    }
} // namespace

std::unique_ptr<compressor> make_compressor_by_id(std::uint8_t id, int level)
{
    std::lock_guard lk(g_registry_mtx);
    if (!g_id_cache_ready) rebuild_id_cache();
    auto it = g_id_to_name.find(id);
    if (it == g_id_to_name.end()) {
        SPDLOG_WARN("No registered compressor for id {:#02x}", id);
        throw std::runtime_error("Unknown compressor id");
    }
    return g_registry[it->second](level);
}

} // namespace arch::compress