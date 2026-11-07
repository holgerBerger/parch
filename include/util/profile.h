#pragma once

// Phase 10: shared per-stage timing mechanism for the reader's extract/verify
// profile and the writer's create/append profile (options.profile). Off cost
// at a call site: one bool test (the helper returns nullptr, the span no-ops).
// Mechanism only — each side owns its stage enums, name tables, summary line,
// and tuning hints.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace arch::util::profile {

inline std::uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

/// Cumulative ns + op count for one pipeline stage (relaxed atomics: exact
/// totals are not needed, torn-free monotonic sums are).
struct stage_timer {
    std::atomic<std::uint64_t> ns{0};
    std::atomic<std::uint64_t> ops{0};
    void add(std::uint64_t dt) {
        ns.fetch_add(dt, std::memory_order_relaxed);
        ops.fetch_add(1, std::memory_order_relaxed);
    }
    void reset() { ns.store(0); ops.store(0); }
};

/// RAII: records elapsed ns into the timer on destruction. `t == nullptr`
/// (profiling off, or stage not measured on this path) is free.
class span {
    stage_timer* t_;
    std::uint64_t t0_;
public:
    explicit span(stage_timer* t)
        : t_(t), t0_(t ? now_ns() : 0) {}
    ~span() { if (t_) t_->add(now_ns() - t0_); }
    span(const span&) = delete; span& operator=(const span&) = delete;
};

/// Shared report body: one table row per stage with ops > 0. `names` is a
/// parallel array of display labels (stage name tables pad to a fixed width
/// so the columns line up). Returns total busy ns across all stages.
inline std::uint64_t append_stage_table(
    std::string& out, const stage_timer* stages, const char* const* names,
    std::size_t count)
{
    std::uint64_t busy = 0;
    for (std::size_t i = 0; i < count; ++i) busy += stages[i].ns.load();
    char line[256];
    for (std::size_t i = 0; i < count; ++i) {
        const auto ns = stages[i].ns.load();
        const auto ops = stages[i].ops.load();
        if (!ns && !ops) continue;
        std::snprintf(line, sizeof line,
                      "  %s %8.3f s %5.1f%% of busy  (%llu ops, %.1f us/op)\n",
                      names[i], ns / 1e9, busy ? 100.0 * ns / busy : 0.0,
                      (unsigned long long)ops, ops ? ns / 1000.0 / ops : 0.0);
        out += line;
    }
    return busy;
}

} // namespace arch::util::profile
