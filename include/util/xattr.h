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

/// fd-based variant (Phase 10): fsetxattr on an already-open descriptor —
/// avoids the extra path lookup on network filesystems. `fd` is not closed.
void write_xattrs(int fd, std::string_view blob);

/// Write POSIX ACLs from the read_acl blob (slot 0 = access, slot 1 =
/// default; terminated by an empty string). Plain getfacl text works too.
/// Failures are logged, never thrown (fs may lack ACL support).
void write_acl(std::filesystem::path path, std::string_view acl_text);

/// fd-based variant: applies the access ACL (slot 0) via acl_set_fd.
/// A non-empty default-ACL slot is ignored with a debug log (regular files
/// cannot carry default ACLs; directories keep the path-based route).
void write_acl(int fd, std::string_view acl_text);

} // namespace arch::util
