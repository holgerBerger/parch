#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace arch::util {

/// Read xattrs from a file. Appends binary blob to `out`.
/// Returns offset into `out`, or 0 if no xattrs.
std::uint64_t read_xattrs(std::filesystem::path path, std::vector<std::uint8_t>& out);

/// Read POSIX ACL from a file (libacl). Appends the blob described in
/// docs/format.md §ACL Storage — `access_text NUL [default_text NUL]` in
/// POSIX.1e text form with numeric IDs — to `out`.
/// Returns offset into `out`, or 0 if no (non-trivial) ACL.
std::uint64_t read_acl(std::filesystem::path path, std::vector<std::uint8_t>& out);

/// Write xattrs to a file from our binary blob format.
void write_xattrs(std::filesystem::path path, std::string_view blob);

/// Write POSIX ACLs from the read_acl blob (slot 0 = access, slot 1 =
/// default; terminated by an empty string). Plain getfacl text works too.
/// Failures are logged, never thrown (fs may lack ACL support).
void write_acl(std::filesystem::path path, std::string_view acl_text);

} // namespace arch::util
