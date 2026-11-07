/*
 * arch_bench — Phase 9 benchmark + memory-profiling harness (PLAN item 32/33).
 *
 * Generates deterministic workloads on local storage, times the in-process
 * arch writer/reader APIs, and (optionally) the external `tar --zstd`
 * reference implementation on the same tree. Peak RSS and minor page faults
 * come from getrusage — child numbers via RUSAGE_CHILDREN around system().
 *
 * One machine-readable result line per run:
 *   tool=<name> workload=<w> op=<o> threads=<n> depth=<n> delay_us=<n>
 *   in_bytes=<n> out_bytes=<n> secs=<f> rate_mibs=<f> maxrss_kib=<n>
 *   minflt=<n> [extras...]
 *
 * LD_PRELOAD of bench/netfs_delay.c (built as libnetfs_delay.so) simulates
 * network-FS latency: every open under NETFS_PREFIX and the first pread per
 * fd sleep NETFS_DELAY_US. Both arch (in-process) and tar (child) are
 * penalised equally.
 */
#include "arch/archive.h"
#include "arch/format.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <random>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static double rss_kib_self() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<double>(ru.ru_maxrss);
}
static long minflt_self() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt;
}

struct time_ctx {
    clk::time_point t0;
    double rss_before;
    long flt_before;
    struct rusage cru_before{};
    void start() {
        t0 = clk::now();
        rss_before = rss_kib_self();
        flt_before = minflt_self();
        getrusage(RUSAGE_CHILDREN, &cru_before);
    }
    double secs() const { return std::chrono::duration<double>(clk::now() - t0).count(); }
    // Peak RSS including any children run since start() (tar subprocesses).
    double rss_peak() const {
        struct rusage ru{};
        getrusage(RUSAGE_SELF, &ru);
        struct rusage cru{};
        getrusage(RUSAGE_CHILDREN, &cru);
        double self = ru.ru_maxrss;
        // RUSAGE_CHILDREN maxrss is the max over ALL past children, so take
        // the delta against itself only when it exceeds the self peak.
        if (static_cast<double>(cru.ru_maxrss) > self) self = cru.ru_maxrss;
        return self;
    }
    long minflt_delta() const {
        struct rusage ru{};
        getrusage(RUSAGE_SELF, &ru);
        return ru.ru_minflt - flt_before;
    }
};

// ---------------------------------------------------------------------------
// workload generation (deterministic)
// ---------------------------------------------------------------------------

// ~40% compressible "text": random words from a small vocabulary.
static void fill_text(std::mt19937_64& rng, std::vector<uint8_t>& buf, size_t n) {
    static const char* words[] = {"alpha", "bravo", "charlie", "delta", "echo",
                                  "foxtrot", "golf", "hotel", "india", "juliet",
                                  "kilo", "lima", "mike", "november", "oscar"};
    constexpr size_t nw = sizeof words / sizeof words[0];
    std::uniform_int_distribution<size_t> pick(0, nw - 1);
    size_t off = 0;
    while (off < n) {
        const char* w = words[pick(rng)];
        size_t l = strlen(w);
        if (off + l + 1 > n) { buf[off++] = 'x'; continue; }
        memcpy(buf.data() + off, w, l);
        off += l;
        buf[off++] = (rng() % 20 == 0) ? '\n' : ' ';
    }
}

struct gen_stats { uint64_t bytes = 0; size_t files = 0; };

static gen_stats gen_tree(const fs::path& root, const std::string& workload) {
    gen_stats g;
    std::vector<uint8_t> buf(4u << 20);
    auto write_file = [&](const fs::path& p, size_t bytes, uint64_t seed,
                          bool text) {
        std::mt19937_64 rng(seed);
        std::string data = p.string();
        fs::create_directories(p.parent_path());
        int fd = ::open(data.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) { perror("open"); exit(1); }
        size_t off = 0;
        while (off < bytes) {
            size_t n = std::min<size_t>(buf.size(), bytes - off);
            if (text) fill_text(rng, buf, n);
            else for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(rng());
            ssize_t w = ::pwrite(fd, buf.data(), n, static_cast<off_t>(off));
            if (w != (ssize_t)n) { perror("pwrite"); exit(1); }
            off += n;
        }
        ::close(fd);
        g.bytes += bytes;
        g.files++;
    };

    fs::remove_all(root);
    fs::create_directories(root);

    if (workload == "small") {
        // 20 000 × 4 KiB text files in 20 dirs — the pipeline's raison d'être
        // (per-file open+read round-trips, nothing else).
        for (int i = 0; i < 20000; ++i) {
            char name[128];
            snprintf(name, sizeof name, "d%02d/f%05d.txt", i / 1000, i);
            write_file(root / name, 4096, 1000 + (uint64_t)i, true);
        }
    } else if (workload == "mixed") {
        // ~512 MiB: big text + big random + mid + many small (real-tree mix).
        for (int i = 0; i < 25; ++i) {
            char name[128];
            snprintf(name, sizeof name, "big/text_%02d.dat", i);
            write_file(root / name, 12u << 20, 5000 + i, true);
            snprintf(name, sizeof name, "big/rand_%02d.dat", i);
            write_file(root / name, 8u << 20, 9000 + i, false);
        }
        for (int i = 0; i < 200; ++i) {
            char name[128];
            snprintf(name, sizeof name, "mid/m_%03d.dat", i);
            write_file(root / name, (uint64_t)(64 + (i % 32)) * 4096, 20000 + i,
                       i % 3 == 0);
        }
        for (int i = 0; i < 2000; ++i) {
            char name[128];
            snprintf(name, sizeof name, "small/s%04d.txt", i);
            write_file(root / name, 2048 + (uint64_t)(i % 7) * 512, 30000 + i, true);
        }
    } else if (workload == "big") {
        // One 1 GiB half-text / half-random file — pure block-path throughput.
        write_file(root / "single.dat", 512u << 20, 77, true);
        // (append half via second file keeps generation simple; the pair is
        //  one text-heavy + one random-heavy 512 MiB file each)
        write_file(root / "single_rand.dat", 512u << 20, 78, false);
    } else {
        fprintf(stderr, "unknown workload: %s\n", workload.c_str());
        exit(2);
    }
    return g;
}

// ---------------------------------------------------------------------------
// runs
// ---------------------------------------------------------------------------

// RSS cost of the writer's dedup containers (dedup_index vector<dedup_node>
// + unordered_map<digest128,uint64>), measured directly at 2 M entries —
// extrapolates to real archives where a 4 MiB block granularity makes the
// delta invisible at benchmark data sizes.
static void run_dedup_ram() {
    auto rss = [] {
        FILE* f = fopen("/proc/self/statm", "r");
        long pages = 0, dummy = 0;
        if (f) { if (fscanf(f, "%ld %ld", &dummy, &pages) < 2) pages = 0; fclose(f); }
        return (double)pages * sysconf(_SC_PAGESIZE) / 1024.0;  // KiB
    };
    struct node { std::uint8_t hash[16]; std::uint64_t off; std::uint64_t idx;
                  std::uint32_t refs; };
    const auto before = rss();
    std::vector<node> index;
    std::unordered_map<std::string, uint64_t> lookup;  // 16-byte key string
    index.reserve(2000000);
    lookup.reserve(2000000);
    std::mt19937_64 rng(1);
    for (uint64_t i = 0; i < 2000000; ++i) {
        node n{};
        memcpy(n.hash, &i, 8);
        uint64_t h2 = rng();
        memcpy(n.hash + 8, &h2, 8);
        n.off = i * (4u << 20);
        n.idx = i;
        n.refs = 1;
        index.push_back(n);
        lookup[std::string(reinterpret_cast<char*>(n.hash), 16)] = i;
    }
    const auto after = rss();
    std::printf(
        "tool=arch workload=- op=dedupram entries=2000000 rss_delta_kib=%.0f "
        "kib_per_entry=%.2f gib_per_pib_unique=%.1f\n",
        after - before, (after - before) / 2000000.0,
        (after - before) / 2000000.0 * (1ull << 28) / (1024.0 * 1024.0));
    std::fflush(stdout);
    // keep containers alive past the measurement
    if (index.empty() || lookup.empty()) std::puts("unreachable");
}

struct cfg {
    std::string workload = "small", op = "all", compressor = "zstd";
    int threads = 4;            // writer::options.threads / reader threads
    std::size_t depth = 32;     // pipeline_depth
    int level = 3;
    bool dedup = false;
    bool sparse = true;
    bool run_tar = true;
    fs::path data = "/home/hobel/benchdata";
};

static fs::path tree_dir(const cfg& c) { return c.data / c.workload / "tree"; }
static fs::path arc_path(const cfg& c, const std::string& who) {
    return c.data / c.workload / (who + ".arc");
}

static void report(const char* tool, const cfg& c, const char* o, uint64_t inb,
                   uint64_t outb, const time_ctx& t) {
    double s = t.secs();
    std::printf(
        "tool=%s workload=%s op=%s threads=%d depth=%zu compressor=%s dedup=%d "
        "in_bytes=%llu out_bytes=%llu secs=%.3f rate_mibs=%.1f "
        "maxrss_kib=%.0f minflt=%ld\n",
        tool, c.workload.c_str(), o, c.threads, c.depth, c.compressor.c_str(),
        (int)c.dedup, (unsigned long long)inb, (unsigned long long)outb, s,
        s > 0 ? (double)inb / (1 << 20) / s : 0.0, t.rss_peak(),
        t.minflt_delta());
    std::fflush(stdout);
}

static uint64_t dir_bytes(const fs::path& p) {
    uint64_t n = 0;
    for (auto& e : fs::recursive_directory_iterator(p))
        if (e.is_regular_file()) n += e.file_size();
    return n;
}

static void run_arch(const cfg& c, uint64_t inb) {
    const auto arc = arc_path(c, "arch");
    spdlog::set_level(spdlog::level::warn);

    if (c.op == "create" || c.op == "all") {
        time_ctx t; t.start();
        arch::writer::options o;
        o.compressor = c.compressor;
        o.compression_level = c.level;
        o.threads = c.threads;
        o.pipeline_depth = c.depth;
        o.profile = std::getenv("ARCH_BENCH_PROFILE") != nullptr;
        o.dedup = c.dedup;
        o.sparse = c.sparse;
        arch::writer w(arc, o);
        w.add_tree(tree_dir(c));
        w.finalize();
        report("arch", c, "create", inb, fs::file_size(arc), t);
    }
    if (c.op == "extract" || c.op == "all") {
        const auto out = c.data / c.workload / "out_arch";
        fs::remove_all(out);
        time_ctx t; t.start();
        arch::reader::options ro;      // Phase 10: depth = files in flight,
        ro.threads = c.threads;        // threads = decode permits
        ro.depth = static_cast<int>(c.depth);
        ro.profile = std::getenv("ARCH_BENCH_PROFILE") != nullptr;
        arch::reader r(arc, ro);
        r.extract_all(out);
        report("arch", c, "extract", inb, 0, t);
    }
    if (c.op == "verify" || c.op == "all") {
        time_ctx t; t.start();
        arch::reader::options ro;
        ro.threads = c.threads;
        ro.depth = static_cast<int>(c.depth);
        ro.profile = std::getenv("ARCH_BENCH_PROFILE") != nullptr;
        arch::reader r(arc, ro);
        auto errs = r.verify_all();
        double s = t.secs();
        std::printf(
            "tool=arch workload=%s op=verify threads=%d depth=%zu "
            "compressor=%s dedup=%d in_bytes=%llu out_bytes=0 secs=%.3f "
            "rate_mibs=%.1f maxrss_kib=%.0f minflt=%ld errors=%zu\n",
            c.workload.c_str(), c.threads, c.depth, c.compressor.c_str(),
            (int)c.dedup, (unsigned long long)inb, s,
            s > 0 ? (double)inb / (1 << 20) / s : 0.0, t.rss_peak(),
            t.minflt_delta(), errs.size());
        std::fflush(stdout);
    }
}

static void run_tar(const cfg& c, uint64_t inb) {
    const fs::path parent = c.data / c.workload;
    const auto tarc = parent / "ref.tar.zst";
    std::string cmd;
    time_ctx t;

    // create — GNU tar shells out to `zstd`; ZSTD_CLEVEL pins the level to
    // ours so the comparison is like-for-like.
    t.start();
    cmd = "ZSTD_CLEVEL=" + std::to_string(c.level) +
          " tar --zstd -cf " + tarc.string() + " -C " + parent.string() + " tree";
    if (std::system(cmd.c_str()) != 0) { fprintf(stderr, "tar create failed\n"); return; }
    report("tar-zstd", c, "create", inb, fs::file_size(tarc), t);

    // extract
    const auto tout = parent / "out_tar";
    fs::remove_all(tout);
    fs::create_directories(tout);
    t.start();
    cmd = "tar --zstd -xf " + tarc.string() + " -C " + tout.string();
    if (std::system(cmd.c_str()) != 0) { fprintf(stderr, "tar extract failed\n"); return; }
    report("tar-zstd", c, "extract", inb, 0, t);

    // store mode (no compression) — the fair counterpart of -c none
    const auto tarc0 = parent / "ref.tar";
    t.start();
    cmd = "tar -cf " + tarc0.string() + " -C " + parent.string() + " tree";
    if (std::system(cmd.c_str()) != 0) { fprintf(stderr, "tar store failed\n"); return; }
    report("tar-store", c, "create", inb, fs::file_size(tarc0), t);
}

int main(int argc, char** argv) {
    cfg c;
    bool gen = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char* key) -> const char* {
            size_t k = strlen(key);
            return a.compare(0, k, key) == 0 ? a.c_str() + k + 1 : nullptr;
        };
        if (auto v = val("--workload")) c.workload = v;
        else if (auto v = val("--op")) c.op = v;
        else if (auto v = val("--threads")) c.threads = atoi(v);
        else if (auto v = val("--depth")) c.depth = (size_t)atol(v);
        else if (auto v = val("--compressor")) c.compressor = v;
        else if (auto v = val("--level")) c.level = atoi(v);
        else if (auto v = val("--data")) c.data = v;
        else if (a == "--dedup") c.dedup = true;
        else if (a == "--no-sparse") c.sparse = false;
        else if (a == "--no-tar") c.run_tar = false;
        else if (a == "--no-gen") gen = false;
        else if (a == "--gen-only") { c.op = "gen"; }
        else { fprintf(stderr, "bad arg: %s\n", a.c_str()); return 2; }
    }
    // The writer's threads knob also gates the pipeline; keep depth sane.
    fs::create_directories(c.data);
    if (gen) {
        auto g = gen_tree(tree_dir(c), c.workload);
        std::printf("gen workload=%s files=%zu bytes=%llu\n", c.workload.c_str(),
                    g.files, (unsigned long long)g.bytes);
        std::fflush(stdout);
    }
    if (c.op == "gen") return 0;
    if (c.op == "dedupram") { run_dedup_ram(); return 0; }

    const uint64_t inb = dir_bytes(tree_dir(c));
    if (c.run_tar) run_tar(c, inb);
    run_arch(c, inb);
    return 0;
}
