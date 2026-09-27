// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal: atomic filesystem publication.
//
// Every durable mutation of authoritative state follows the same sequence:
// stage into the store's tmp directory, flush the staged bytes, rename into the
// committed location, then flush the containing directory so the rename itself
// survives a crash. The helpers here implement that sequence for both files and
// directory-level renames, and they report the operating system error rather
// than silently degrading to a non-atomic path.

#ifndef ASSET_REGISTRY_INTERNAL_FS_ATOMIC_HPP
#define ASSET_REGISTRY_INTERNAL_FS_ATOMIC_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "asset_registry/error.hpp"

namespace asset_registry::internal {

/// Operating system error text for the most recent failure on this thread.
[[nodiscard]] std::string last_system_error();

/// Reads an entire file into memory. Fails with StoreNotFound when the path does
/// not exist, StoreIoError when it cannot be read, and StoreIntegrityFailed when
/// the file exceeds `max_bytes`. The size check runs against the stat result
/// before any buffer is allocated, so an enormous file cannot exhaust memory.
[[nodiscard]] Outcome<std::string> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes);

/// Writes bytes to `path`, creating parent directories as needed, then flushes
/// the file contents to the storage device. The write is not atomic; callers that
/// need atomicity stage into a temporary and use publish_file().
Outcome<void> write_file_flushed(const std::filesystem::path& path, std::string_view bytes);

/// Atomically replaces `destination` with `source`. Both must be on the same
/// volume; the store layout guarantees this by staging inside the store
/// directory. Flushes the containing directory afterwards on platforms where a
/// directory handle can be flushed.
Outcome<void> publish_replace(const std::filesystem::path& source, const std::filesystem::path& destination);

/// Flushes a directory entry so a rename or create inside it is durable where the
/// platform supports it. A no-op on platforms without directory flushing.
Outcome<void> sync_directory(const std::filesystem::path& directory);

/// Writes `bytes` into `directory/tmp/<name>` with the process and a random
/// suffix, flushes it, and returns the staged path. On any failure the staged
/// file is removed before returning.
[[nodiscard]] Outcome<std::filesystem::path> stage_file(const std::filesystem::path& tmp_directory,
                                                        std::string_view name, std::string_view bytes);

/// Removes a file, reporting StoreIoError on failure other than "not found".
Outcome<void> remove_file(const std::filesystem::path& path);

/// Lists regular files directly inside `directory`, sorted ascending by filename.
/// Returns an empty vector when the directory does not exist.
[[nodiscard]] Outcome<std::vector<std::string>> list_files(const std::filesystem::path& directory);

/// Creates a directory and every missing parent. Idempotent.
Outcome<void> ensure_directory(const std::filesystem::path& directory);

/// True when `path` exists and is a regular file.
[[nodiscard]] bool is_regular_file(const std::filesystem::path& path) noexcept;

/// Rejects path components that would escape the store directory. Store layout
/// names are fixed by the format, so a correctly built path never contains a
/// separator or a traversal element; this predicate makes that a checked
/// property rather than an assumption.
[[nodiscard]] bool is_safe_relative_name(std::string_view name) noexcept;

}  // namespace asset_registry::internal

#endif  // ASSET_REGISTRY_INTERNAL_FS_ATOMIC_HPP
