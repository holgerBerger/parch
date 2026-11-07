/*
 * archiver – CLI (PLAN.md Phase 5, item 25).
 *
 *   arch create  [opts] <archive> <tree|file...>   build a new archive
 *   arch append  [opts] <archive> <tree|file...>   add a section to one
 *   arch list    [opts] <archive>                  show entries
 *   arch extract [opts] <archive> [dest]           unpack (dest = ".")
 *
 * Writer/reader options map 1:1 to arch::writer::options /
 * arch::reader::options (docs/api.md). Paths that are directories go through
 * add_tree (names stored relative to the directory); plain file arguments are
 * stored by their basename (writer::add_file semantics — same-basename files
 * from different directories collide, like tar without leading dirs).
 */
#include "arch/archive.h"
#include "arch/compressor.h"
#include "arch/format.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h> // also declares stderr_color_st
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <ctime>

namespace {

struct cli_opts {
    // writer side
    std::string compressor = "zstd";
    int level = 3;
    int threads = 4;              // -j: also seeds the extract pool
    std::size_t depth = 32;       // pipeline_depth
    bool dedup = false;
    bool durable = true;
    bool threads_given = false;   // reader default = hardware concurrency
    // ui
    spdlog::level::level_enum log_level = spdlog::level::info;
};

[[noreturn]] void usage_error(const std::string& msg, int code = 2) {
    if (!msg.empty()) spdlog::error("{}", msg);
    std::FILE* sink = code == 0 ? stdout : stderr;
    std::fprintf(sink,
        "usage: arch create  [opts] <archive> <tree|file...>\n"
        "       arch append  [opts] <archive> <tree|file...>\n"
        "       arch list    [opts] <archive>\n"
        "       arch extract [opts] <archive> [dest]      (dest default: .)\n"
        "\nopts:\n"
        "  -c, --compressor NAME   zstd (default) or none (store)\n"
        "  -l, --level N           compression level (default 3)\n"
        "  -j, --threads N         compression / extraction workers (default 4)\n"
        "      --depth N           files in flight in the I/O pipeline (default 32)\n"
        "      --dedup             within-section content dedup (XXH3-128)\n"
        "      --no-sync           skip fsync barriers (faster, less durable)\n"
        "  -v, --verbose           debug logging\n"
        "  -q, --quiet             errors only\n"
        "      --version           print version and exit\n"
        "  -h, --help              this text\n");
    std::exit(code);
}

std::uint64_t parse_u64(std::string_view s, const char* what) {
    std::uint64_t v = 0;
    const auto* b = s.data();
    const auto* e = b + s.size();
    auto [p, ec] = std::from_chars(b, e, v);
    if (ec != std::errc{} || p != e)
        usage_error(std::string(what) + ": bad number '" + std::string(s) + "'");
    return v;
}

// Parses the shared option flags; returns positional (non-option) arguments.
// Options after `--` are treated as positional.
std::vector<std::string> parse_args(int argc, char** argv, cli_opts& o) {
    std::vector<std::string> pos;
    bool no_more_opts = false;
    auto need_value = [&](int& i, const char* opt) -> std::string {
        if (++i >= argc) usage_error(std::string("option ") + opt + " needs a value");
        return argv[i];
    };
    for (int i = 0; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (no_more_opts || a.empty() || a.front() != '-') { pos.emplace_back(a); continue; }
        if (a == "--") { no_more_opts = true; continue; }
        if (a == "-c" || a == "--compressor") o.compressor = need_value(i, "-c");
        else if (a == "-l" || a == "--level")  o.level = static_cast<int>(parse_u64(need_value(i, "-l"), "--level"));
        else if (a == "-j" || a == "--threads") { o.threads = static_cast<int>(parse_u64(need_value(i, "-j"), "--threads")); o.threads_given = true; }
        else if (a == "--depth") o.depth = static_cast<std::size_t>(parse_u64(need_value(i, "--depth"), "--depth"));
        else if (a == "--dedup") o.dedup = true;
        else if (a == "--no-sync") o.durable = false;
        else if (a == "-v" || a == "--verbose") o.log_level = spdlog::level::debug;
        else if (a == "-q" || a == "--quiet") o.log_level = spdlog::level::err;
        else if (a == "-h" || a == "--help") { usage_error("", 0); }
        else if (a == "--version") {
            std::printf("arch %s (format v0.2)\n",
#ifdef ARCH_CLI_VERSION
                        ARCH_CLI_VERSION
#else
                        "(dev)"
#endif
            );
            std::exit(0);
        }
        else usage_error("unknown option: " + std::string(a));
    }
    return pos;
}

arch::writer::options writer_opts(const cli_opts& o) {
    arch::writer::options w;
    w.compressor = o.compressor;
    w.compression_level = o.level;
    w.threads = o.threads;
    w.pipeline_depth = o.depth;
    w.dedup = o.dedup;
    w.durable = o.durable;
    return w;
}

struct source_stats {
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    std::uint64_t dirs = 0;
    std::uint64_t others = 0; // symlinks/devices/fifos
};

source_stats stat_sources(const std::vector<std::string>& args) {
    namespace fs = std::filesystem;
    source_stats s;
    for (const auto& a : args) {
        std::error_code ec;
        if (fs::is_directory(a, ec)) {
            for (fs::recursive_directory_iterator it(a, fs::directory_options::skip_permission_denied, ec), end;
                 it != end; it.increment(ec)) {
                if (ec) continue;
                std::error_code sec;
                const auto type = it->symlink_status(sec).type();
                if (sec) continue;
                if (type == fs::file_type::directory) ++s.dirs;
                else if (type == fs::file_type::regular) {
                    s.bytes += static_cast<std::uint64_t>(fs::file_size(*it, ec));
                    if (!ec) ++s.files;
                } else ++s.others;
            }
        } else if (fs::is_regular_file(a, ec)) {
            s.bytes += static_cast<std::uint64_t>(fs::file_size(a, ec));
            if (!ec) ++s.files;
        } else if (fs::exists(a, ec)) {
            ++s.others; // add_file on a symlink/device stores the target's data
        } else {
            throw std::system_error(ENOENT, std::generic_category(), a);
        }
    }
    return s;
}

std::uint64_t file_size_of(const std::filesystem::path& p) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(p, ec);
    return ec ? 0 : n;
}

using clock_t_ = std::chrono::steady_clock;

double elapsed_s(const clock_t_::time_point& t0) {
    return std::chrono::duration<double>(clock_t_::now() - t0).count();
}

std::string human(std::uint64_t bytes) {
    const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    char buf[48];
    std::snprintf(buf, sizeof buf, i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
    return buf;
}

// ---- commands ----

int cmd_write(bool append_mode, std::vector<std::string> args, const cli_opts& o) {
    if (args.size() < 2) usage_error("need <archive> and at least one path");
    const std::filesystem::path arc = args.front();
    args.erase(args.begin());

    for (const auto& a : args) // fail fast on missing inputs
        if (!std::filesystem::exists(a))
            throw std::system_error(ENOENT, std::generic_category(), a);

    const auto src = stat_sources(args);
    spdlog::info("arch{} {}: {} file(s), {} dir(s), {} other entr(ies), {} input",
                 append_mode ? "ive append" : " creating", arc.string(),
                 src.files, src.dirs, src.others, human(src.bytes));

    const auto t0 = clock_t_::now();
    if (append_mode) {
        auto w = arch::writer::append(arc, writer_opts(o));
        for (const auto& a : args)
            std::filesystem::is_directory(a) ? w.add_tree(a) : w.add_file(a);
        w.finalize();
    } else {
        arch::writer w(arc, writer_opts(o));
        for (const auto& a : args)
            std::filesystem::is_directory(a) ? w.add_tree(a) : w.add_file(a);
        w.finalize();
    }
    const double secs = elapsed_s(t0);

    const std::uint64_t out = file_size_of(arc);
    if (append_mode)
        spdlog::info("done in {:.2f}s — archive now {}", secs, human(out));
    else
        spdlog::info("done in {:.2f}s — archive {} ({:.1f}% of input)", secs,
                     human(out),
                     src.bytes ? 100.0 * static_cast<double>(out) /
                                     static_cast<double>(src.bytes) : 0.0);
    return 0;
}

int cmd_list(std::vector<std::string> args, const cli_opts& o) {
    if (args.size() != 1) usage_error("usage: arch list <archive>");
    arch::reader r(args.front(), arch::reader::options{});
    const auto entries = r.list();

    std::uint64_t total = 0;
    for (const auto& e : entries) {
        char t = '-';
        switch (e.type) {
            case arch::format::file_type::directory: t = 'd'; break;
            case arch::format::file_type::symlink:   t = 'l'; break;
            case arch::format::file_type::blkdev:    t = 'b'; break;
            case arch::format::file_type::chrdev:    t = 'c'; break;
            case arch::format::file_type::fifo:      t = 'p'; break;
            case arch::format::file_type::sock:      t = 's'; break;
            default: break;
        }
        char perms[10] = "xxxxxxxxx";
        for (int bit = 0; bit < 9; ++bit)
            perms[bit] = (e.mode & (1u << (8 - bit))) ? "rwx"[bit % 3] : '-';
        char when[32] = {};
        const std::time_t tt = static_cast<std::time_t>(e.mtime);
        std::tm tmv{};
        localtime_r(&tt, &tmv);
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tmv);
        if (e.type == arch::format::file_type::regular) total += e.size;
        std::printf("%c%s %8llu %s  %s%s%s\n", t, perms,
                    static_cast<unsigned long long>(e.size), when,
                    e.path.string().c_str(),
                    e.link_target.empty() ? "" : " -> ", e.link_target.c_str());
    }
    if (o.log_level <= spdlog::level::debug)
        std::fprintf(stderr, "%zu entries, %s in regular files\n",
                     entries.size(), human(total).c_str());
    return 0;
}

int cmd_extract(std::vector<std::string> args, const cli_opts& o) {
    if (args.empty() || args.size() > 2) usage_error("usage: arch extract <archive> [dest]");
    const std::filesystem::path arc = args.front();
    const std::filesystem::path dest = args.size() > 1 ? args[1] : ".";

    arch::reader::options ro;
    if (o.threads_given) ro.threads = o.threads;
    arch::reader r(arc, ro);

    std::size_t n = r.list().size();
    const auto t0 = clock_t_::now();
    r.extract_all(dest);
    spdlog::info("extracted {} entr{} to {} in {:.2f}s", n, n == 1 ? "y" : "ies",
                 dest.string(), elapsed_s(t0));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Diagnostics/progress on stderr; `list` output on stdout (scriptable).
    spdlog::set_default_logger(spdlog::stderr_color_st("arch"));
    // Command-first or options-first both work: `arch create a.arc src` and
    // `arch -q create a.arc src` parse identically (first positional = cmd).
    cli_opts o;
    auto pos = parse_args(argc - 1, argv + 1, o);
    spdlog::set_level(o.log_level);
    if (pos.empty()) usage_error("no command given");
    const std::string cmd = pos.front();
    std::vector<std::string> args(pos.begin() + 1, pos.end());

    try {
        if (cmd == "create") return cmd_write(false, std::move(args), o);
        if (cmd == "append") return cmd_write(true, std::move(args), o);
        if (cmd == "list")   return cmd_list(std::move(args), o);
        if (cmd == "extract") return cmd_extract(std::move(args), o);
        usage_error("unknown command: " + cmd);
    } catch (const std::exception& e) {
        spdlog::error("{}", e.what());
        return 1;
    }
}
