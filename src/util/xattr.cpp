#include "util/xattr.h"
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

        // Write our binary format: [4-byte total_len, 2-byte name_len, name, 4-byte val_len, val]
        auto total = static_cast<std::uint32_t>(sizeof(std::uint32_t) + 2 + name.size() + sizeof(std::uint32_t) + vsz);
        out.resize(out.size() + total);
        std::memcpy(out.data() + out.size() - total, &total, 4);
        auto nlen = static_cast<std::uint16_t>(name.size());
        std::memcpy(out.data() + out.size() - total + 4, &nlen, 2);
        std::memcpy(out.data() + out.size() - total + 6, name.data(), name.size());
        auto vlen = static_cast<std::uint32_t>(vsz);
        std::memcpy(out.data() + out.size() - total + 6 + name.size(), &vlen, 4);
        std::memcpy(out.data() + out.size() - total + 6 + name.size() + 4, val.data(), vsz);
    }

    // Terminator: 4-byte zero length.
    std::uint32_t zero = 0;
    out.resize(out.size() + 4);
    std::memcpy(out.data() + out.size() - 4, &zero, 4);

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
        std::uint32_t total;
        std::memcpy(&total, p, 4);
        if (total == 0) break; // terminator

        std::uint16_t nlen;
        std::memcpy(&nlen, p + 4, 2);
        std::string_view name(reinterpret_cast<const char*>(p + 6), nlen);

        std::uint32_t vlen;
        std::memcpy(&vlen, p + 6 + nlen, 4);
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
