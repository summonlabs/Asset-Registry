// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "uuid.hpp"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
// bcrypt.h must follow windows.h.
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace asset_registry::internal {
namespace {

class Sha1 {
public:
    Sha1() { reset(); }

    void reset() noexcept {
        h_[0] = 0x67452301U;
        h_[1] = 0xEFCDAB89U;
        h_[2] = 0x98BADCFEU;
        h_[3] = 0x10325476U;
        h_[4] = 0xC3D2E1F0U;
        total_bytes_ = 0;
        buffer_size_ = 0;
    }

    void update(const std::uint8_t* data, std::size_t size) noexcept {
        if (size == 0) {
            return;
        }
        total_bytes_ += size;
        std::size_t offset = 0;
        if (buffer_size_ != 0) {
            while (offset < size && buffer_size_ < 64) {
                buffer_[buffer_size_++] = data[offset++];
            }
            if (buffer_size_ == 64) {
                process_block(buffer_.data());
                buffer_size_ = 0;
            }
        }
        while (size - offset >= 64) {
            process_block(data + offset);
            offset += 64;
        }
        while (offset < size) {
            buffer_[buffer_size_++] = data[offset++];
        }
    }

    [[nodiscard]] Sha1Digest finish() noexcept {
        const std::uint64_t bit_length = total_bytes_ * 8U;

        // Padding: 0x80, then zeros, then the 64-bit big-endian message length,
        // ending on a 64-byte block boundary. When the terminator leaves no room
        // for the length field the block is closed and a second block is filled.
        buffer_[buffer_size_++] = 0x80U;
        if (buffer_size_ > 56) {
            while (buffer_size_ < kBlockBytes) {
                buffer_[buffer_size_++] = 0x00U;
            }
            process_block(buffer_.data());
            buffer_size_ = 0;
        }
        while (buffer_size_ < 56) {
            buffer_[buffer_size_++] = 0x00U;
        }
        for (std::size_t index = 0; index < 8; ++index) {
            buffer_[buffer_size_++] = static_cast<std::uint8_t>((bit_length >> ((7U - index) * 8U)) & 0xFFU);
        }
        process_block(buffer_.data());
        buffer_size_ = 0;

        Sha1Digest digest{};
        for (std::size_t word = 0; word < 5; ++word) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
                digest[word * 4 + byte] = static_cast<std::uint8_t>((h_[word] >> ((3U - byte) * 8U)) & 0xFFU);
            }
        }
        return digest;
    }

private:
    static constexpr std::size_t kBlockBytes = 64;

    [[nodiscard]] static std::uint32_t rotate_left(std::uint32_t value, unsigned amount) noexcept {
        return (value << amount) | (value >> (32U - amount));
    }

    void process_block(const std::uint8_t* block) noexcept {
        std::uint32_t words[80];
        for (std::size_t index = 0; index < 16; ++index) {
            words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24U) |
                           (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16U) |
                           (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8U) |
                           static_cast<std::uint32_t>(block[index * 4 + 3]);
        }
        for (std::size_t index = 16; index < 80; ++index) {
            words[index] = rotate_left(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
        }

        std::uint32_t a = h_[0];
        std::uint32_t b = h_[1];
        std::uint32_t c = h_[2];
        std::uint32_t d = h_[3];
        std::uint32_t e = h_[4];

        for (std::size_t index = 0; index < 80; ++index) {
            std::uint32_t function = 0;
            std::uint32_t constant = 0;
            if (index < 20) {
                function = (b & c) | ((~b) & d);
                constant = 0x5A827999U;
            } else if (index < 40) {
                function = b ^ c ^ d;
                constant = 0x6ED9EBA1U;
            } else if (index < 60) {
                function = (b & c) | (b & d) | (c & d);
                constant = 0x8F1BBCDCU;
            } else {
                function = b ^ c ^ d;
                constant = 0xCA62C1D6U;
            }
            const std::uint32_t temp = rotate_left(a, 5) + function + e + constant + words[index];
            e = d;
            d = c;
            c = rotate_left(b, 30);
            b = a;
            a = temp;
        }

        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    std::uint32_t h_[5]{};
    std::uint64_t total_bytes_ = 0;
    std::size_t buffer_size_ = 0;
    std::array<std::uint8_t, kBlockBytes> buffer_{};
};

/// Operating-system entropy. Returns false rather than degrading to a predictable
/// source, because a predictable canonical AssetId would be a real defect.
[[nodiscard]] bool fill_random(std::uint8_t* destination, std::size_t size) noexcept {
#ifdef _WIN32
    const NTSTATUS status = ::BCryptGenRandom(nullptr, destination, static_cast<ULONG>(size),
                                              BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0;
#else
    const int descriptor = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t got = ::read(descriptor, destination + offset, size - offset);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) {
                continue;
            }
            ::close(descriptor);
            return false;
        }
        offset += static_cast<std::size_t>(got);
    }
    ::close(descriptor);
    return true;
#endif
}

}  // namespace

Sha1Digest sha1(const std::uint8_t* data, std::size_t size) noexcept {
    Sha1 hasher;
    hasher.update(data, size);
    return hasher.finish();
}

Sha1Digest sha1(std::string_view data) noexcept {
    return sha1(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

Sha1Digest sha1_parts(std::initializer_list<std::string_view> parts) noexcept {
    Sha1 hasher;
    for (const std::string_view part : parts) {
        hasher.update(reinterpret_cast<const std::uint8_t*>(part.data()), part.size());
    }
    return hasher.finish();
}

const AssetId::bytes_type& asset_namespace_uuid() noexcept {
    // A fixed, arbitrarily chosen namespace for this registry. Documented in the
    // README as part of the deterministic-identity contract.
    static const AssetId::bytes_type value = {0x6b, 0x2f, 0x1d, 0x4c, 0x8e, 0x37, 0x4a, 0x11,
                                              0x9c, 0x2b, 0x02, 0x42, 0xac, 0x11, 0x00, 0x07};
    return value;
}

AssetId uuid_v5(const AssetId::bytes_type& name_space, std::string_view name) noexcept {
    std::array<std::uint8_t, 16> input{};
    std::memcpy(input.data(), name_space.data(), 16);
    Sha1 hasher;
    hasher.update(input.data(), input.size());
    hasher.update(reinterpret_cast<const std::uint8_t*>(name.data()), name.size());
    const Sha1Digest digest = hasher.finish();

    AssetId::bytes_type bytes{};
    std::memcpy(bytes.data(), digest.data(), 16);
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x50U);  // version 5
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);  // RFC 4122 variant
    return AssetId::from_bytes(bytes);
}

Outcome<AssetId> uuid_v4() {
    AssetId::bytes_type bytes{};
    if (!fill_random(bytes.data(), bytes.size())) {
        return make_error(ErrorCode::AllocationFailed,
                          "operating system entropy source is unavailable; refusing to generate a "
                          "predictable canonical asset identity");
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);  // version 4
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);  // RFC 4122 variant
    return AssetId::from_bytes(bytes);
}

std::array<std::uint8_t, 16> request_fingerprint(std::string_view domain, std::string_view payload) noexcept {
    std::array<std::uint8_t, 16> result{};
    const Sha1Digest digest = sha1_parts({domain, std::string_view("\x1f", 1), payload});
    std::memcpy(result.data(), digest.data(), result.size());
    return result;
}

}  // namespace asset_registry::internal
