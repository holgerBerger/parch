#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace arch::util {

/// Read xattrs from a file. Appends binary blob to `out`.
/// Returns offset into `out`, or 0 if no xattrs.
std::uint64_t read_xattrs(std::filesystem::path path, std::vector<std::uint8_t>& out);

/// Read POSIX ACL from a file. Appends text blob to `out`.
/// Returns offset into `out`, or 0 if no ACL.
std::uint64_t read_acl(std::filesystem::path path, std::vector<std::uint8_t>& out);

/// Write xattrs to a file from our binary blob format.
void write_xattrs(std::filesystem::path path, std::string_view blob);

/// Write POSIX ACL to a file from getfacl/restorefacl text.
void write_acl(std::filesystem::path path, std::string_view acl_text);

} // namespace arch::util
