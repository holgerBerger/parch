#include "util/xattr.h"
#include "arch/format.h"
#include <spdlog/spdlog.h>
#include <errno.h>
#include <sys/xattr.h>
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
        result.emplace_back(p, static_cast<std::size_t>(nul - p));
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

std::uint64_t read_acl(std::filesystem::path path, std::vector<std::uint8_t>& out) {
    // Read ACL via getfattr or fallback to getfacl command.
    // For now: use getfacl program.
    // TODO: use libacl if available.
    return 0;
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

void write_acl(std::filesystem::path path, std::string_view acl_text) {
    // TODO: use setfacl or libacl.
    (void)path;
    (void)acl_text;
}

} // namespace arch::util
