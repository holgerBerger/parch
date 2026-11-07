#include "util/xattr.h"
#include "arch/format.h"
#include <spdlog/spdlog.h>
#include <errno.h>
#include <sys/xattr.h>
#include <sys/stat.h>
#include <sys/acl.h>
#include <acl/libacl.h>
#include <linux/limits.h>
#include <string.h>
#include <cstring>
#include <algorithm>

namespace arch::util {

static std::vector<std::string> list_xattrs(std::filesystem::path path) {
    std::vector<std::string> result;
    // Linux: listxattr returns comma-separated names.
    ssize_t total = ::listxattr(path.c_str(), nullptr, 0);
    if (total < 0) return result;

    std::vector<char> buf(total + 1);
    if (::listxattr(path.c_str(), buf.data(), total) < 0) return result;

    // Split by NUL.
    const char* p = buf.data();
    const char* end = buf.data() + total;
    while (p < end) {
        const char* nul = static_cast<const char*>(memchr(p, '\0', end - p));
        if (!nul) nul = end;
        std::string_view name(p, static_cast<std::size_t>(nul - p));
        // POSIX ACLs live in system.posix_acl_{access,default} xattrs under
        // the hood; they get the dedicated acl_id field, not the xattr blob.
        if (name != "system.posix_acl_access" && name != "system.posix_acl_default")
            result.emplace_back(name);
        p = nul + 1;
    }
    return result;
}

std::uint64_t read_xattrs(std::filesystem::path path, std::vector<std::uint8_t>& out) {
    auto xattrs = list_xattrs(path);
    if (xattrs.empty()) return 0;

    std::uint64_t off = out.size();

    for (auto& name : xattrs) {
        ssize_t vsz = ::getxattr(path.c_str(), name.c_str(), nullptr, 0);
        if (vsz < 0) continue;

        std::vector<char> val(vsz);
        if (::getxattr(path.c_str(), name.c_str(), val.data(), vsz) < 0) continue;

        // Spec format (docs/format.md §Xattr Storage), big-endian:
        // [4-byte total_len, 2-byte name_len, name, 4-byte val_len, val]
        auto total = static_cast<std::size_t>(4 + 2 + name.size() + 4) + static_cast<std::size_t>(vsz);
        out.resize(out.size() + total);
        std::uint8_t* p = out.data() + out.size() - total;
        auto x32 = arch::format::htobe32_safe(static_cast<std::uint32_t>(total));
        std::memcpy(p, &x32, 4);
        auto x16 = arch::format::htobe16_safe(static_cast<std::uint16_t>(name.size()));
        std::memcpy(p + 4, &x16, 2);
        std::memcpy(p + 6, name.data(), name.size());
        auto v32 = arch::format::htobe32_safe(static_cast<std::uint32_t>(vsz));
        std::memcpy(p + 6 + name.size(), &v32, 4);
        std::memcpy(p + 6 + name.size() + 4, val.data(), vsz);
    }

    // Terminator: 4-byte zero length.
    out.resize(out.size() + 4);
    std::memset(out.data() + out.size() - 4, 0, 4);

    return off;
}

namespace {

// One NUL-terminated ACL text (access or default) → acl_set_file.
void apply_acl_text(const std::filesystem::path& path, std::string_view text,
                    acl_type_t type) {
    const std::string z(text);
    acl_t a = ::acl_from_text(z.c_str());
    if (!a) {
        SPDLOG_WARN("acl_from_text failed for {} ({}): {}", path.c_str(),
                    type == ACL_TYPE_DEFAULT ? "default" : "access",
                    strerror(errno));
        return;
    }
    // Stored texts normally carry an explicit mask; synthesize one only when
    // named entries make the ACL extended (acl_set_file rejects those
    // without a mask — libacl does not compute it the way setfacl does).
    // A base-3-entry text must stay mask-less so it restores as a trivial
    // (mode-only) ACL instead of growing a spurious mask entry.
    bool has_mask = false, has_named = false;
    acl_entry_t e;
    for (int idx = 0;
         ::acl_get_entry(a, idx == 0 ? ACL_FIRST_ENTRY : ACL_NEXT_ENTRY, &e) == 1;
         ++idx) {
        acl_tag_t tag;
        if (::acl_get_tag_type(e, &tag) != 0) continue;
        if (tag == ACL_MASK) { has_mask = true; break; }
        if (tag == ACL_USER || tag == ACL_GROUP) has_named = true;
    }
    if (!has_mask && has_named && ::acl_calc_mask(&a) != 0)
        SPDLOG_DEBUG("acl_calc_mask failed for {}: {}", path.c_str(), strerror(errno));

    if (::acl_set_file(path.c_str(), type, a) != 0) {
        // A filesystem without ACL support must not fail extraction; EPERM
        // happens restoring onto a file we do not own.
        if (errno == EOPNOTSUPP || errno == ENOTSUP || errno == ENOSYS || errno == EPERM)
            SPDLOG_DEBUG("acl_set_file {} ({}) unsupported: {}", path.c_str(),
                         type == ACL_TYPE_DEFAULT ? "default" : "access", strerror(errno));
        else
            SPDLOG_WARN("acl_set_file {} failed: {}", path.c_str(), strerror(errno));
    }
    ::acl_free(a);
}

// ACL text of an fd-relative posix ACL, NUL-free, no trailing newline.
// Numeric IDs (TEXT_NUMERIC_IDS) so archives restore on hosts that lack
// the source user/group names.
std::string acl_text(acl_t a) {
    std::string s;
    if (char* t = ::acl_to_any_text(a, nullptr, '\n', TEXT_NUMERIC_IDS)) {
        s.assign(t);
        ::acl_free(t);
        while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    }
    return s;
}

} // namespace

// Blob layout (docs/format.md §ACL Storage), written by read_acl and
// completed by the writer's trailing push_back('\0'):
//
//   access_text NUL [default_text NUL]        (terminated by empty string)
//
// The access part is present whenever anything is stored (trivial base text
// when the access ACL is mode-equivalent) so the positional parse is
// unambiguous. A blob of "access\0" (access only) and "base\0def\0" (with a
// default ACL) both terminate at the writer's extra NUL.
std::uint64_t read_acl(std::filesystem::path path, std::vector<std::uint8_t>& out) {
    std::string access, def;
    struct stat st{};
    const bool have_st = ::stat(path.c_str(), &st) == 0;

    if (acl_t a = ::acl_get_file(path.c_str(), ACL_TYPE_ACCESS)) {
        // Trivial (mode-equivalent) access ACLs are reconstructible from the
        // mode bits — only store them as a positional placeholder when a
        // default ACL forces us to keep the access slot.
        const bool trivial = ::acl_equiv_mode(a, nullptr) == 0;
        if (!trivial) access = acl_text(a);
        acl_free(a);
    } else if (!have_st || !S_ISDIR(st.st_mode)) {
        return 0; // no ACL support (ENOTSUP) or ENODATA on a plain file
    }

    if (have_st && S_ISDIR(st.st_mode)) {
        if (acl_t d = ::acl_get_file(path.c_str(), ACL_TYPE_DEFAULT)) {
            def = acl_text(d);
            acl_free(d);
        }
    }

    if (access.empty() && def.empty()) return 0;
    if (access.empty()) {
        // Placeholder from the mode bits so `def` lands in slot 1.
        static const char* rwx[] = {"---", "--x", "-w-", "-wx",
                                    "r--", "r-x", "rw-", "rwx"};
        auto bits = [](mode_t m, int o) { return rwx[(m >> o) & 7]; };
        access = std::string("u::") + bits(st.st_mode, 6) + "\ng::" +
                 bits(st.st_mode, 3) + "\no::" + bits(st.st_mode, 0);
    }

    const std::uint64_t off = out.size();
    out.insert(out.end(), access.begin(), access.end());
    out.push_back('\0');
    if (!def.empty()) {
        out.insert(out.end(), def.begin(), def.end());
        out.push_back('\0');
    }
    return off;
}

void write_xattrs(std::filesystem::path path, std::string_view blob) {
    auto* p = reinterpret_cast<const std::uint8_t*>(blob.data());
    auto* end = p + blob.size();

    while (p + 4 <= end) {
        std::uint32_t total_be, total;
        std::memcpy(&total_be, p, 4);
        total = arch::format::betoh32_safe(total_be);
        if (total == 0) break; // terminator

        std::uint16_t nlen_be, nlen;
        std::memcpy(&nlen_be, p + 4, 2);
        nlen = arch::format::betoh16_safe(nlen_be);
        std::string_view name(reinterpret_cast<const char*>(p + 6), nlen);

        std::uint32_t vlen_be, vlen;
        std::memcpy(&vlen_be, p + 6 + nlen, 4);
        vlen = arch::format::betoh32_safe(vlen_be);
        auto* val = p + 6 + nlen + 4;

        ::setxattr(path.c_str(), name.data(), val, vlen, 0);

        p += total;
    }
}

void write_acl(std::filesystem::path path, std::string_view blob) {
    // Slot 0: access ACL text; slot 1 (optional): default ACL text. The list
    // ends at an empty string (the writer's terminator) or the blob's end —
    // trailing string-table bytes are never consumed (self-terminating blob,
    // same contract as write_xattrs).
    for (const acl_type_t type : {ACL_TYPE_ACCESS, ACL_TYPE_DEFAULT}) {
        const auto nul = blob.find('\0');
        const std::string_view text =
            nul == std::string_view::npos ? blob : blob.substr(0, nul);
        if (text.empty()) break;
        apply_acl_text(path, text, type);
        if (nul == std::string_view::npos) break;
        blob = blob.substr(nul + 1);
    }
}

} // namespace arch::util
