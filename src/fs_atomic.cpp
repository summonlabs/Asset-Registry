// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "fs_atomic.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace asset_registry::internal {
namespace {

constexpr std::size_t kCopyBufferBytes = 1U << 16U;

[[nodiscard]] std::string describe_error(const std::error_code& code) {
    return code.message() + " (system error " + std::to_string(code.value()) + ")";
}

}  // namespace

std::string last_system_error() {
    const std::error_code code(errno, std::generic_category());
    return describe_error(code);
}

Outcome<std::string> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(path, error);
    if (error) {
        if (!std::filesystem::exists(path)) {
            return make_error(ErrorCode::StoreNotFound, "file does not exist: " + path.string());
        }
        return make_error(ErrorCode::StoreIoError, "cannot stat file: " + path.string() + ": " + describe_error(error));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return make_error(ErrorCode::StoreLayoutInvalid, "path is not a regular file: " + path.string());
    }
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        return make_error(ErrorCode::StoreIoError, "cannot size file: " + path.string() + ": " + describe_error(error));
    }
    if (size > max_bytes) {
        return make_error(ErrorCode::StoreIntegrityFailed,
                          "file exceeds the permitted size: " + path.string() + " is " + std::to_string(size) +
                              " bytes, limit is " + std::to_string(max_bytes));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return make_error(ErrorCode::StoreIoError, "cannot open file for reading: " + path.string());
    }
    std::string contents;
    contents.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        stream.read(contents.data(), static_cast<std::streamsize>(size));
        if (!stream) {
            return make_error(ErrorCode::StoreIoError, "short read on file: " + path.string());
        }
    }
    return contents;
}

Outcome<void> ensure_directory(const std::filesystem::path& directory) {
    std::error_code error;
    if (std::filesystem::exists(directory, error)) {
        if (error) {
            return make_error(ErrorCode::StoreIoError,
                              "cannot query directory: " + directory.string() + ": " + describe_error(error));
        }
        if (!std::filesystem::is_directory(directory)) {
            return make_error(ErrorCode::StoreLayoutInvalid,
                              "path exists but is not a directory: " + directory.string());
        }
        return Outcome<void>{};
    }
    std::filesystem::create_directories(directory, error);
    if (error) {
        return make_error(ErrorCode::StoreIoError,
                          "cannot create directory: " + directory.string() + ": " + describe_error(error));
    }
    return Outcome<void>{};
}

bool is_regular_file(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool is_safe_relative_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 255) {
        return false;
    }
    if (name == "." || name == "..") {
        return false;
    }
    for (const char character : name) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '/' || character == '\\' || character == ':') {
            return false;
        }
        if (byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    return true;
}

Outcome<void> write_file_flushed(const std::filesystem::path& path, std::string_view bytes) {
#ifdef _WIN32
    const HANDLE handle =
        ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return make_error(ErrorCode::StoreIoError,
                          "cannot create file: " + path.string() + ": " + last_system_error());
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, 1U << 30U));
        DWORD produced = 0;
        if (::WriteFile(handle, bytes.data() + written, chunk, &produced, nullptr) == 0) {
            ::CloseHandle(handle);
            return make_error(ErrorCode::StoreIoError,
                              "write failed: " + path.string() + ": " + last_system_error());
        }
        if (produced == 0) {
            ::CloseHandle(handle);
            return make_error(ErrorCode::StoreIoError, "write made no progress: " + path.string());
        }
        written += produced;
    }
    if (::FlushFileBuffers(handle) == 0) {
        ::CloseHandle(handle);
        return make_error(ErrorCode::StoreIoError,
                          "flush failed: " + path.string() + ": " + last_system_error());
    }
    ::CloseHandle(handle);
    return Outcome<void>{};
#else
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        return make_error(ErrorCode::StoreIoError,
                          "cannot create file: " + path.string() + ": " + last_system_error());
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t produced = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (produced < 0) {
            if (errno == EINTR) {
                continue;
            }
            const std::string detail = last_system_error();
            ::close(descriptor);
            return make_error(ErrorCode::StoreIoError, "write failed: " + path.string() + ": " + detail);
        }
        written += static_cast<std::size_t>(produced);
    }
    if (::fsync(descriptor) != 0) {
        const std::string detail = last_system_error();
        ::close(descriptor);
        return make_error(ErrorCode::StoreIoError, "flush failed: " + path.string() + ": " + detail);
    }
    if (::close(descriptor) != 0) {
        return make_error(ErrorCode::StoreIoError, "close failed: " + path.string() + ": " + last_system_error());
    }
    return Outcome<void>{};
#endif
}

Outcome<void> publish_replace(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
    if (::MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return make_error(ErrorCode::StorePublishFailed,
                          "atomic replace failed: " + source.string() + " -> " + destination.string() + ": " +
                              last_system_error());
    }
    return Outcome<void>{};
#else
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        return make_error(ErrorCode::StorePublishFailed,
                          "atomic replace failed: " + source.string() + " -> " + destination.string() + ": " +
                              last_system_error());
    }
    return sync_directory(destination.parent_path());
#endif
}

Outcome<void> sync_directory(const std::filesystem::path& directory) {
#ifdef _WIN32
    // Windows has no supported way to flush a directory entry. Durability of the
    // rename itself relies on MOVEFILE_WRITE_THROUGH in publish_replace, which is
    // the documented mechanism on this platform.
    (void)directory;
    return Outcome<void>{};
#else
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return make_error(ErrorCode::StoreIoError,
                          "cannot open directory for flush: " + directory.string() + ": " + last_system_error());
    }
    if (::fsync(descriptor) != 0) {
        const std::string detail = last_system_error();
        ::close(descriptor);
        return make_error(ErrorCode::StoreIoError,
                          "directory flush failed: " + directory.string() + ": " + detail);
    }
    ::close(descriptor);
    return Outcome<void>{};
#endif
}

Outcome<std::filesystem::path> stage_file(const std::filesystem::path& tmp_directory, std::string_view name,
                                          std::string_view bytes) {
    if (!is_safe_relative_name(name)) {
        return make_error(ErrorCode::StoreLayoutInvalid, "unsafe staging file name");
    }
    std::error_code error;
    std::filesystem::create_directories(tmp_directory, error);
    if (error) {
        return make_error(ErrorCode::StoreIoError,
                          "cannot create staging directory: " + tmp_directory.string() + ": " +
                              describe_error(error));
    }
    // A unique staging name keeps concurrent recoverers from colliding even
    // though only one writer holds the store lock at a time.
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t serial = counter.fetch_add(1, std::memory_order_relaxed);
    std::string unique_name(name);
    unique_name += ".stage.";
    unique_name += std::to_string(serial);
    const std::filesystem::path staged = tmp_directory / unique_name;

    const Outcome<void> write_result = write_file_flushed(staged, bytes);
    if (!write_result) {
        std::filesystem::remove(staged, error);
        return write_result.error();
    }
    return staged;
}

Outcome<void> remove_file(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::remove(path, error)) {
        return Outcome<void>{};
    }
    if (error) {
        return make_error(ErrorCode::StoreIoError, "cannot remove file: " + path.string() + ": " + describe_error(error));
    }
    // Nothing to remove is success: removal is idempotent by design.
    return Outcome<void>{};
}

Outcome<std::vector<std::string>> list_files(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        return names;
    }
    if (error) {
        return make_error(ErrorCode::StoreIoError,
                          "cannot query directory: " + directory.string() + ": " + describe_error(error));
    }
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        return make_error(ErrorCode::StoreIoError,
                          "cannot enumerate directory: " + directory.string() + ": " + describe_error(error));
    }
    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error) {
            continue;
        }
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace asset_registry::internal
