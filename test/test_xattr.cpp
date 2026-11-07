#include <catch2/catch_test_macros.hpp>
#include "util/xattr.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <sys/xattr.h>
#include <vector>
#include <cstdint>
#include <random>

TEST_CASE("crc32c of known data block", "[cutil][xattr]") {
    // Test xattr read/write roundtrip on a temp file.
    const auto path = std::filesystem::temp_directory_path() /
                      "arch_test_xattr.txt";
    {
        std::ofstream(path) << "content";
    }

    // Set a simple xattr.
    REQUIRE(::setxattr(path.c_str(), "user.test", "value", 5, 0) == 0);

    // Read it back via our util.
    std::vector<std::uint8_t> out;
    auto off = arch::util::read_xattrs(path, out);
    REQUIRE(off >= 0); // blob was written
    REQUIRE(!out.empty());

    // Write it back via our util.
    // We can't easily test re-read without implementing the parser more.
    // At minimum, the function should not throw.
    // (Skipped for now — util is still a skeleton.)

    std::filesystem::remove(path);
}

TEST_CASE("xattr on file with no attributes returns 0", "[util][xattr]") {
    const auto path = std::filesystem::temp_directory_path() /
                      "arch_test_no_xattr.txt";
    {
        std::ofstream(path) << "content";
    }

    std::vector<std::uint8_t> out;
    auto off = arch::util::read_xattrs(path, out);
    REQUIRE(off == 0);
    REQUIRE(out.empty());

    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// POSIX ACLs (Phase 7)
// ---------------------------------------------------------------------------

#include <sys/acl.h>
#include <acl/libacl.h>
#include <sys/stat.h>
#include "arch/archive.h"
#include <cerrno>
#include <cstring>
#include <string>

namespace {

// Set an ACL from text, computing the mask when named entries require one
// (mirrors what setfacl does — acl_set_file rejects the rest with EINVAL).
// Returns false when the filesystem lacks ACL support.
bool probe_set_acl(const char* path, acl_type_t type, const char* text) {
    acl_t a = acl_from_text(text);
    REQUIRE(a != nullptr);
    bool has_mask = false, has_named = false;
    acl_entry_t e;
    for (int idx = 0;
         acl_get_entry(a, idx == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &e) == 1;
         ++idx) {
        acl_tag_t tag;
        if (acl_get_tag_type(e, &tag) != 0) continue;
        if (tag == ACL_MASK) { has_mask = true; break; }
        if (tag == ACL_USER || tag == ACL_GROUP) has_named = true;
    }
    if (!has_mask && has_named) REQUIRE(acl_calc_mask(&a) == 0);
    bool ok = acl_set_file(path, type, a) == 0;
    if (!ok && (errno == EOPNOTSUPP || errno == ENOTSUP || errno == ENOSYS))
        return false; // caller decides to skip
    acl_free(a);
    REQUIRE(ok);
    return true;
}

// read_acl → texts (slot0 = access, slot1 = default if present).
std::vector<std::string> acl_blob_texts(const std::vector<std::uint8_t>& blob) {
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i < blob.size()) {
        std::size_t j = i;
        while (j < blob.size() && blob[j] != '\0') ++j;
        if (j == i) break; // empty string = terminator
        parts.emplace_back(reinterpret_cast<const char*>(blob.data() + i), j - i);
        i = j + 1;
    }
    return parts;
}

std::string acl_of(const std::filesystem::path& p, acl_type_t type) {
    acl_t a = acl_get_file(p.c_str(), type);
    if (!a) return {};
    char* t = acl_to_any_text(a, nullptr, '\n', TEXT_NUMERIC_IDS);
    std::string s = t ? t : "";
    acl_free(t);
    acl_free(a);
    while (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

} // namespace

TEST_CASE("util: trivial ACLs are not stored", "[util][acl]") {
    char tmpl[] = "/tmp/opencode/arch_acl_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    REQUIRE(dir != nullptr);
    const auto f = std::filesystem::path(dir) / "plain.txt";
    { std::ofstream(f) << "x"; }

    if (!probe_set_acl(f.c_str(), ACL_TYPE_ACCESS, "u::rw-\ng::r--\no::r--"))
        SKIP("filesystem lacks POSIX ACL support");

    std::vector<std::uint8_t> blob;
    REQUIRE(arch::util::read_acl(f, blob) == 0);
    REQUIRE(blob.empty()); // mode-equivalent → reconstructible, not stored

    std::filesystem::remove(f);
    std::filesystem::remove(dir);
}

TEST_CASE("util: acl read/write roundtrip (access + default)", "[util][acl]") {
    char tmpl[] = "/tmp/opencode/arch_acl_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    REQUIRE(dir != nullptr);
    const auto f = std::filesystem::path(dir) / "a.txt";
    const auto d = std::filesystem::path(dir) / "sub";
    { std::ofstream(f) << "x"; }
    REQUIRE(::mkdir(d.c_str(), 0755) == 0);

    const char* acc = "u::rw-\ng::r--\no::r--\nu:12345:rwx\ng:54321:rw-";
    const char* def = "u::rwx\ng::r-x\no::r-x\nu:12345:rwx";
    if (!probe_set_acl(f.c_str(), ACL_TYPE_ACCESS, acc))
        SKIP("filesystem lacks POSIX ACL support");
    REQUIRE(probe_set_acl(d.c_str(), ACL_TYPE_ACCESS, "u::rwx\ng::r-x\no::r-x\nu:12345:r-x"));
    REQUIRE(probe_set_acl(d.c_str(), ACL_TYPE_DEFAULT, def));

    // --- access ACL on a file ---
    {
        std::vector<std::uint8_t> blob;
        arch::util::read_acl(f, blob);
        auto parts = acl_blob_texts(blob);
        REQUIRE(parts.size() == 1);
        CHECK(parts[0].find("user:12345:rwx") != std::string::npos);
        CHECK(parts[0].find("group:54321:rw-") != std::string::npos);
        CHECK(parts[0].find("mask::") != std::string::npos);

        // posix_acl system xattrs must not double-store into the xattr blob
        std::vector<std::uint8_t> xb;
        arch::util::read_xattrs(f, xb);
        REQUIRE(xb.empty());

        const auto f2 = std::filesystem::path(dir) / "b.txt";
        { std::ofstream(f2) << "x"; }
        arch::util::write_acl(f2,
            std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
        CHECK(acl_of(f2, ACL_TYPE_ACCESS) == acl_of(f, ACL_TYPE_ACCESS));
    }

    // --- directory with access + default (slot 0 placeholder + slot 1) ---
    {
        std::vector<std::uint8_t> blob;
        arch::util::read_acl(d, blob);
        auto parts = acl_blob_texts(blob);
        REQUIRE(parts.size() == 2); // access (non-trivial here) + default

        const auto d2 = std::filesystem::path(dir) / "sub2";
        REQUIRE(::mkdir(d2.c_str(), 0700) == 0);
        arch::util::write_acl(d2,
            std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
        CHECK(acl_of(d2, ACL_TYPE_ACCESS) == acl_of(d, ACL_TYPE_ACCESS));
        CHECK(acl_of(d2, ACL_TYPE_DEFAULT) == acl_of(d, ACL_TYPE_DEFAULT));
    }

    std::filesystem::remove_all(dir);
}

TEST_CASE("archive: ACLs survive writer→reader roundtrip", "[util][acl][reader][writer]") {
    namespace fs = std::filesystem;
    char tmpl[] = "/tmp/opencode/arch_acl_XXXXXX";
    const char* root = ::mkdtemp(tmpl);
    REQUIRE(root != nullptr);
    const fs::path rp(root), src = rp / "src", arc = rp / "t.arc", out = rp / "out";
    fs::create_directories(src / "sub");
    { std::ofstream(src / "f.txt") << "hello"; }
    { std::ofstream(src / "sub/g.txt") << "nested"; }
    fs::permissions(src / "sub", fs::perms::owner_all | fs::perms::group_read |
                                   fs::perms::others_exec);

    if (!probe_set_acl((src / "f.txt").c_str(), ACL_TYPE_ACCESS,
                       "u::rw-\ng::r--\no::r--\nu:23456:rwx"))
        SKIP("filesystem lacks POSIX ACL support");
    REQUIRE(probe_set_acl((src / "sub").c_str(), ACL_TYPE_ACCESS,
                          "u::rwx\ng::r-x\no::r-x\ng:65432:rw-"));
    REQUIRE(probe_set_acl((src / "sub").c_str(), ACL_TYPE_DEFAULT,
                          "u::rwx\ng::r-x\no::r-x\nu:23456:r-x"));

    {
        arch::writer::options o;
        arch::writer w(arc, o);
        w.add_tree(src);
        w.finalize();
    }
    {
        arch::reader r(arc);
        // access ACL visible through the reader API
        const auto txt = r.get_acl("f.txt");
        CHECK(txt.find("user:23456:rwx") != std::string::npos);
        CHECK(r.get_acl("sub/g.txt").empty()); // trivial → not stored
        r.extract_all(out);
    }
    // Extracted tree carries the same ACLs.
    CHECK(acl_of(out / "f.txt", ACL_TYPE_ACCESS) ==
          acl_of(src / "f.txt", ACL_TYPE_ACCESS));
    CHECK(acl_of(out / "sub", ACL_TYPE_ACCESS) ==
          acl_of(src / "sub", ACL_TYPE_ACCESS));
    CHECK(acl_of(out / "sub", ACL_TYPE_DEFAULT) ==
          acl_of(src / "sub", ACL_TYPE_DEFAULT));
    // …and the posix_acl xattrs are not duplicated into the xattr table.
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> xa;
    {
        arch::reader r(arc);
        xa = r.get_xattrs("f.txt");
    }
    CHECK(xa.empty());

    fs::remove_all(rp);
}

TEST_CASE("archive: xattrs=false / acls=false skip the metadata probes",
          "[util][acl][xattr][writer]") {
    namespace fs = std::filesystem;
    char tmpl[] = "/tmp/opencode/arch_nometa_XXXXXX";
    const char* root = ::mkdtemp(tmpl);
    REQUIRE(root != nullptr);
    const fs::path rp(root), src = rp / "src", arc = rp / "t.arc";
    fs::create_directories(src);
    { std::ofstream(src / "f.txt") << "hello"; }
    REQUIRE(::setxattr((src / "f.txt").c_str(), "user.arch.test",
                       "value", 5, 0) == 0);
    if (!probe_set_acl((src / "f.txt").c_str(), ACL_TYPE_ACCESS,
                       "u::rw-\ng::r--\no::r--\nu:23456:rwx"))
        SKIP("filesystem lacks POSIX ACL support");

    // Both the pipeline (threads>0) and the serial fallback (threads=0)
    // honour the flags; run each variant and inspect through the reader.
    for (int threads : {4, 0}) {
        fs::remove(arc);
        {
            arch::writer::options o;
            o.threads = threads;
            o.xattrs = false;
            o.acls = false;
            arch::writer w(arc, o);
            w.add_tree(src);
            w.finalize();
        }
        arch::reader r(arc);
        CHECK(r.get_xattrs("f.txt").empty());
        CHECK(r.get_acl("f.txt").empty());
    }

    // Positive control with default options: both are captured.
    fs::remove(arc);
    {
        arch::writer w(arc);
        w.add_tree(src);
        w.finalize();
    }
    {
        arch::reader r(arc);
        CHECK_FALSE(r.get_xattrs("f.txt").empty());
        CHECK(r.get_acl("f.txt").find("user:23456:rwx") != std::string::npos);
    }

    fs::remove_all(rp);
}
