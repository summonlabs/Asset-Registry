// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "crc32.hpp"

#include <array>

namespace asset_registry::internal {
namespace {

/// Standard reflected CRC-32 table, generated once at first use. Construction is
/// thread safe under the C++11 and later guarantee for block-scope statics.
[[nodiscard]] const std::array<std::uint32_t, 256>& crc_table() noexcept {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> generated{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0U ? (value >> 1U) ^ 0xEDB88320U : (value >> 1U);
            }
            generated[index] = value;
        }
        return generated;
    }();
    return table;
}

// All polynomial arithmetic below is expressed in the reflected representation:
// the generator polynomial is 0xEDB88320, bit i of a value holds the coefficient
// of x^(31-i), and multiplication shifts right with feedback. Keeping one
// representation throughout avoids the class of error where a reflected table is
// combined with a non-reflected reduction.

[[nodiscard]] std::uint32_t gf2_multiply(std::uint32_t lhs, std::uint32_t rhs) noexcept {
    std::uint32_t result = 0;
    std::uint32_t a = lhs;
    std::uint32_t b = rhs;
    for (int bit = 0; bit < 32; ++bit) {
        if ((b & 1U) != 0U) {
            result ^= a;
        }
        const bool low_bit = (a & 1U) != 0U;
        a >>= 1U;
        if (low_bit) {
            a ^= 0xEDB88320U;
        }
        b >>= 1U;
    }
    return result;
}

/// x^n in the reflected representation: x^0 is bit 31 (0x80000000) and each
/// increment of the exponent shifts the coefficient one position toward the low
/// bit, reducing modulo the generator on the way.
[[nodiscard]] std::uint32_t gf2_x_pow(std::uint64_t exponent) noexcept {
    std::uint32_t result = 0x80000000U;  // x^0
    std::uint32_t base = 0x40000000U;    // x^1
    std::uint64_t remaining = exponent;
    while (remaining != 0) {
        if ((remaining & 1U) != 0U) {
            result = gf2_multiply(result, base);
        }
        base = gf2_multiply(base, base);
        remaining >>= 1U;
    }
    return result;
}

[[nodiscard]] std::uint32_t update(std::uint32_t state, const std::uint8_t* data, std::size_t size) noexcept {
    const auto& table = crc_table();
    std::uint32_t crc = state;
    for (std::size_t index = 0; index < size; ++index) {
        crc = table[(crc ^ data[index]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc;
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
    return update(0xFFFFFFFFU, data, size) ^ 0xFFFFFFFFU;
}

std::uint32_t crc32(std::string_view data) noexcept {
    return crc32(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::uint32_t crc32_pair(std::string_view first, std::string_view second) noexcept {
    std::uint32_t crc = update(0xFFFFFFFFU, reinterpret_cast<const std::uint8_t*>(first.data()), first.size());
    crc = update(crc, reinterpret_cast<const std::uint8_t*>(second.data()), second.size());
    return crc ^ 0xFFFFFFFFU;
}

std::uint32_t crc32_combine(std::uint32_t first, std::uint32_t second, std::uint64_t second_length) noexcept {
    if (second_length == 0) {
        return first;
    }
    const std::uint32_t shifted = gf2_multiply(first ^ 0xFFFFFFFFU, gf2_x_pow(second_length * 8U));
    return (shifted ^ (second ^ 0xFFFFFFFFU)) ^ 0xFFFFFFFFU;
}

std::uint32_t crc32_shift_zeros(std::uint32_t seed, std::uint64_t length) noexcept {
    return crc32_combine(seed, 0, length);
}

std::uint32_t crc32_u64_be(std::uint32_t seed, std::uint64_t value) noexcept {
    std::array<std::uint8_t, 8> encoded{};
    for (std::size_t index = 0; index < 8; ++index) {
        encoded[index] = static_cast<std::uint8_t>((value >> ((7U - index) * 8U)) & 0xFFU);
    }
    return update(seed ^ 0xFFFFFFFFU, encoded.data(), encoded.size()) ^ 0xFFFFFFFFU;
}

}  // namespace asset_registry::internal
