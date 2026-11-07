// Phase 5: CLI end-to-end tests — run the `parch` binary as a subprocess and
// verify exit codes, stdout shape, and full create/list/extract/append
// round-trips (the binary itself is the unit under test, not the library).
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>

#ifndef ARCH_CLI_BINARY
#define ARCH_CLI_BINARY "build/parch"   // override via compile definition
#endif

using namespace std::string_literals;

namespace {

struct temp_dir {
    std::filesystem::path path;
    temp_dir() {
        char tmpl[] = "/tmp/opencode/arch_cli_XXXXXX";
        const char* p = ::mkdtemp(tmpl);
        REQUIRE(p != nullptr);
        path = p;
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

struct result {
    int code;
    std::string out;
    std::string err;
};

result run(const std::string& cmd) {
    result r;
    char outtmpl[] = "/tmp/opencode/.cli_out_XXXXXX";
    char errtmpl[] = "/tmp/opencode/.cli_err_XXXXXX";
    const int ofd = ::mkstemp(outtmpl), efd = ::mkstemp(errtmpl);
    REQUIRE(ofd >= 0);
    REQUIRE(efd >= 0);
    ::close(ofd); ::close(efd);
    const std::string full = cmd + " >" + outtmpl + " 2>" + errtmpl;
    const int rc = std::system(full.c_str());
    r.code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    auto slurp = [](const char* p) {
        std::string s;
        if (FILE* f = std::fopen(p, "rb")) {
            std::array<char, 4096> b;
            std::size_t n;
            while ((n = std::fread(b.data(), 1, b.size(), f)) > 0) s.append(b.data(), n);
            std::fclose(f);
            std::remove(p);
        }
        return s;
    };
    r.out = slurp(outtmpl);
    r.err = slurp(errtmpl);
    return r;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// whole-line match (plain `list` is one path per line)
bool has_line(const std::string& out, const std::string& line) {
    return contains("\n" + out + "\n", "\n" + line + "\n");
}

void write_file(const std::filesystem::path& p, const std::string& content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream os(p, std::ios::binary);
    os << content;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream is(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(is)),
                       std::istreambuf_iterator<char>());
}

std::string cli(const std::string& args) {
    return "'"s ARCH_CLI_BINARY "' "s + args;
}

} // namespace

TEST_CASE("cli: create + list + extract roundtrip", "[cli]") {
    temp_dir td;
    const auto s = td.path / "src";
    write_file(s / "a.txt", "alpha contents");
    write_file(s / "deep/b.bin", std::string(70000, 'Z'));
    std::filesystem::create_symlink("a.txt", s / "link.txt");
    const auto arc = td.path / "t.arc";

    auto r = run(cli("create -q '" + arc.string() + "' '" + s.string() + "'"));
    REQUIRE(r.code == 0);
    REQUIRE(std::filesystem::exists(arc));

    r = run(cli("list '" + arc.string() + "'"));
    REQUIRE(r.code == 0);
    CHECK(r.out == "src\nsrc/a.txt\nsrc/deep\nsrc/deep/b.bin\nsrc/link.txt\n"); // tar tf: paths only
    CHECK_FALSE(contains(r.out, "->"));

    r = run(cli("list -v '" + arc.string() + "'"));
    REQUIRE(r.code == 0);
    CHECK(contains(r.out, "-rw"));              // tar tv: detailed rows
    CHECK(contains(r.out, "src/deep/b.bin"));
    CHECK(contains(r.out, "src/link.txt -> a.txt"));
    CHECK(contains(r.out, "drwx"));

    const auto out = td.path / "out";
    r = run(cli("extract -q '" + arc.string() + "' '" + out.string() + "'"));
    REQUIRE(r.code == 0);
    CHECK(read_file(out / "src/a.txt") == "alpha contents");
    CHECK(read_file(out / "src/deep/b.bin") == std::string(70000, 'Z'));
    CHECK(std::filesystem::read_symlink(out / "src/link.txt") == "a.txt");
}

TEST_CASE("cli: list -v adds owner/group column", "[cli]") {
    temp_dir td;
    write_file(td.path / "src/f.txt", "hello owner");
    const auto arc = td.path / "t.arc";
    REQUIRE(run(cli("create -q '" + arc.string() + "' '" +
                    (td.path / "src").string() + "'")).code == 0);

    // resolve the expected names exactly like the CLI does: numeric fallback
    // when NSS has no entry for our euid/egid.
    std::string owner = std::to_string(geteuid());
    {
        char buf[1024];
        struct passwd pw{};
        struct passwd* res = nullptr;
        if (getpwuid_r(geteuid(), &pw, buf, sizeof buf, &res) == 0 && res)
            owner = pw.pw_name;
    }
    std::string group = std::to_string(getegid());
    {
        char buf[1024];
        struct group gr{};
        struct group* res = nullptr;
        if (getgrgid_r(getegid(), &gr, buf, sizeof buf, &res) == 0 && res)
            group = gr.gr_name;
    }

    // plain list: paths only — no owner column, no detail fields at all
    auto r = run(cli("list '" + arc.string() + "'"));
    REQUIRE(r.code == 0);
    CHECK(r.out == "src\nsrc/f.txt\n");
    CHECK_FALSE(contains(r.out, owner + "/" + group));

    // -v: tar-style "user/group" between the perms and the size
    r = run(cli("list -v '" + arc.string() + "'"));
    REQUIRE(r.code == 0);
    CHECK(contains(r.out, owner + "/" + group));
    CHECK(contains(r.out, "src/f.txt"));
}

TEST_CASE("cli: append adds entries, extraction takes newest", "[cli]") {
    temp_dir td;
    const auto arc = td.path / "t.arc";
    write_file(td.path / "v1.txt", "version one");
    write_file(td.path / "v2.txt", "version two");
    write_file(td.path / "extra.txt", "extra data");

    REQUIRE(run(cli("create -q '" + arc.string() + "' '" +
                    (td.path / "v1.txt").string() + "' '" +
                    (td.path / "v2.txt").string() + "'")).code == 0);
    // overwrite v1.txt via append
    write_file(td.path / "v1.txt", "version TWO NEW");
    REQUIRE(run(cli("append -q '" + arc.string() + "' '" +
                    (td.path / "v1.txt").string() + "' '" +
                    (td.path / "extra.txt").string() + "'")).code == 0);

    auto r = run(cli("list '" + arc.string() + "'"));
    CHECK(contains(r.out, "v1.txt"));
    CHECK(contains(r.out, "extra.txt"));

    const auto out = td.path / "out";
    REQUIRE(run(cli("extract -q '" + arc.string() + "' '" + out.string() + "'")).code == 0);
    CHECK(read_file(out / "v1.txt") == "version TWO NEW"); // last-occurrence wins
    CHECK(read_file(out / "v2.txt") == "version two");
    CHECK(read_file(out / "extra.txt") == "extra data");
}

TEST_CASE("cli: store mode (-c none) and pipeline knobs", "[cli]") {
    temp_dir td;
    const auto s = td.path / "src";
    for (int i = 0; i < 25; ++i)
        write_file(s / ("f" + std::to_string(i) + ".dat"),
                   std::string(100000, static_cast<char>('a' + i % 26)));
    const auto arc = td.path / "s.arc";
    auto r = run(cli("create -q -c none -j 4 --depth 8 '" + arc.string() + "' '" +
                     s.string() + "'"));
    REQUIRE(r.code == 0);
    const auto out = td.path / "out";
    REQUIRE(run(cli("extract -q '" + arc.string() + "' '" + out.string() + "'")).code == 0);
    bool all_ok = true;
    for (int i = 0; i < 25; ++i)
        if (read_file(out / "src" / ("f" + std::to_string(i) + ".dat")) !=
            std::string(100000, static_cast<char>('a' + i % 26)))
            all_ok = false;
    CHECK(all_ok);
}

TEST_CASE("cli: error codes and messages", "[cli]") {
    temp_dir td;
    const auto arc = td.path / "e.arc";
    write_file(td.path / "f.txt", "fine");
    REQUIRE(run(cli("create -q '" + arc.string() + "' '" +
                    (td.path / "f.txt").string() + "'")).code == 0);

    // Unknown flag → usage, rc 2
    auto r = run(cli("--bogus list '" + arc.string() + "'"));
    CHECK(r.code == 2);
    CHECK(contains(r.err, "unknown option"));

    // Unknown command → rc 2
    r = run(cli("frobnicate '" + arc.string() + "'"));
    CHECK(r.code == 2);
    CHECK(contains(r.err, "unknown command"));

    // Missing input file for create → rc 1
    r = run(cli("create -q '" + (td.path / "x.arc").string() + "' /definitely/missing"));
    CHECK(r.code == 1);
    CHECK(contains(r.err, "No such file"));

    // Extract a non-archive → rc 1 (validation throw, clean message)
    auto junk = td.path / "junk.arc";
    write_file(junk, std::string(4096, 'N'));
    r = run(cli("extract -q '" + junk.string() + "' '" + (td.path / "o").string() + "'"));
    CHECK(r.code == 1);
    CHECK(contains(r.err, "footer"));

    // Corrupted data byte → extract fails rc 1 with a CRC message, list still works
    write_file(td.path / "d.bin", std::string(100000, 'Q'));
    const auto arc2 = td.path / "d.arc";
    REQUIRE(run(cli("create -q '" + arc2.string() + "' '" +
                    (td.path / "d.bin").string() + "'")).code == 0);
    { // flip a byte in the first stored block (header 16 + toc_header 128 + 5)
        std::fstream f(arc2, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(16 + 128 + 5);
        const char c = static_cast<char>(f.get() ^ 0x5A);
        f.seekp(16 + 128 + 5);
        f.put(c);
    }
    CHECK(run(cli("list '" + arc2.string() + "'")).code == 0); // metadata intact
    r = run(cli("extract -q '" + arc2.string() + "' '" + (td.path / "o2").string() + "'"));
    CHECK(r.code == 1);
    CHECK(contains(r.err, "CRC"));

    // --help exits 0 and prints usage
    r = run(cli("-h"));
    CHECK(r.code == 0);
    CHECK(contains(r.out, "usage: parch"));

    // --version exits 0 and prints the format version
    r = run(cli("--version"));
    CHECK(r.code == 0);
    CHECK(contains(r.out, "format v0.2"));
}

TEST_CASE("cli: options are accepted before or after the command", "[cli]") {
    temp_dir td;
    const auto arc = td.path / "p.arc";
    write_file(td.path / "p.txt", "positional");
    // opts after command (classic) and opts before command must both work
    CHECK(run(cli("create -q '" + arc.string() + "' '" + (td.path / "p.txt").string() + "'")).code == 0);
    auto r = run(cli("list '" + arc.string() + "'"));
    CHECK(r.code == 0);
    CHECK(contains(r.out, "p.txt"));
}

TEST_CASE("cli: tar-style top dir + -C + contents-only + multi-root", "[cli]") {
    temp_dir td;
    write_file(td.path / "proj/a.txt", "alpha");
    write_file(td.path / "proj/sub/b.txt", "beta");

    SECTION("directory arg stores the top dir (and trailing slash is a no-op)") {
        const auto arc = td.path / "t1.arc";
        REQUIRE(run(cli("create -q '" + arc.string() + "' '" + (td.path / "proj").string() + "'")).code == 0);
        auto r = run(cli("list '" + arc.string() + "'"));
        CHECK(has_line(r.out, "proj")); // the top dir itself is an entry
        CHECK(contains(r.out, "proj/a.txt"));
        CHECK(contains(r.out, "proj/sub/b.txt"));
        const auto out = td.path / "out";
        REQUIRE(run(cli("extract -q '" + arc.string() + "' '" + out.string() + "'")).code == 0);
        CHECK(read_file(out / "proj/a.txt") == "alpha");
        CHECK(read_file(out / "proj/sub/b.txt") == "beta");

        // "proj/" must produce the same entry names as "proj"
        const auto arc2 = td.path / "t2.arc";
        REQUIRE(run(cli("create -q '" + arc2.string() + "' '" + (td.path / "proj").string() + "/'")).code == 0);
        CHECK(run(cli("list '" + arc2.string() + "'")).out == r.out);
    }

    SECTION("-C resolves archive and inputs from DIR (like tar -C)") {
        auto r = run(cli("-C '" + td.path.string() + "' create -q rel.arc proj"));
        REQUIRE(r.code == 0);
        CHECK(std::filesystem::exists(td.path / "rel.arc"));
        r = run(cli("list '" + (td.path / "rel.arc").string() + "'"));
        CHECK(contains(r.out, "proj/a.txt"));
        // missing -C dir → rc 1 with a message
        r = run(cli("-C /definitely/missing create -q x.arc ."));
        CHECK(r.code == 1);
        CHECK(contains(r.err, "current path"));
    }

    SECTION("-C DIR . stores only the contents") {
        const auto arc = td.path / "t3.arc";
        REQUIRE(run(cli("create -q '" + arc.string() + "' -C '" +
                        (td.path / "proj").string() + "' .")).code == 0);
        auto r = run(cli("list '" + arc.string() + "'"));
        CHECK(contains(r.out, "a.txt"));
        CHECK_FALSE(contains(r.out, "proj/a.txt"));
        const auto out = td.path / "out";
        REQUIRE(run(cli("extract -q '" + arc.string() + "' '" + out.string() + "'")).code == 0);
        CHECK(read_file(out / "a.txt") == "alpha");
        CHECK(read_file(out / "sub/b.txt") == "beta");
    }

    SECTION("two directory args no longer collide at the root") {
        write_file(td.path / "d1/conf", "one");
        write_file(td.path / "d2/conf", "two");
        const auto arc = td.path / "t4.arc";
        REQUIRE(run(cli("create -q '" + arc.string() + "' '" + (td.path / "d1").string() +
                        "' '" + (td.path / "d2").string() + "'")).code == 0);
        auto r = run(cli("list '" + arc.string() + "'"));
        CHECK(contains(r.out, "d1/conf"));
        CHECK(contains(r.out, "d2/conf"));
        const auto out = td.path / "out";
        REQUIRE(run(cli("extract -q '" + arc.string() + "' '" + out.string() + "'")).code == 0);
        CHECK(read_file(out / "d1/conf") == "one");
        CHECK(read_file(out / "d2/conf") == "two");
    }
}
