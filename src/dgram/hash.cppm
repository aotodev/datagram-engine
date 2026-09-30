// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file hash.cppm
 * @brief Keyed hashing for demux keys: SipHash, its seed, and the append protocol.
 */
module;

#include <cerrno>
#include <sys/random.h>

export module dgram:hash;

import std;
import :error;

namespace dgram {

/** @brief A 128-bit SipHash key. Two tables with different seeds share no collisions. */
export struct hash_seed {
    std::uint64_t k0{};
    std::uint64_t k1{};

    // Not `= default`: GCC 16.2 crashes mangling a defaulted one when an importer uses it.
    [[nodiscard]] friend constexpr bool operator==(const hash_seed& a, const hash_seed& b) noexcept { return a.k0 == b.k0 && a.k1 == b.k1; }
};

/** @brief A seed from `getrandom`. */
export [[nodiscard]] inline result<hash_seed> random_seed() noexcept {
    std::array<std::uint64_t, 2> words{};
    const auto got{::getrandom(words.data(), sizeof(words), 0)};
    if (got < 0) [[unlikely]] {
        return fail<hash_seed>();
    }
    // The kernel never shortens a read of at most 256 bytes; checked anyway.
    if (static_cast<std::size_t>(got) != sizeof(words)) [[unlikely]] {
        return std::unexpected{errc{EIO}};
    }
    return hash_seed{words[0], words[1]};
}

/**
 * @brief Streaming SipHash-`C`-`D`.
 *
 * Feeding bytes in any split yields the same digest as feeding them at once.
 */
export template <int C, int D> class basic_siphash {
public:
    explicit constexpr basic_siphash(const hash_seed seed) noexcept
        : v0_{seed.k0 ^ 0x736F6D6570736575ULL}, v1_{seed.k1 ^ 0x646F72616E646F6DULL}, v2_{seed.k0 ^ 0x6C7967656E657261ULL},
          v3_{seed.k1 ^ 0x7465646279746573ULL} {}

    constexpr void operator()(const std::span<const std::byte> bytes) noexcept {
        auto rest{bytes};
        while (!rest.empty() && (length_ & 7) != 0) {
            absorb_byte(rest.front());
            rest = rest.subspan(1);
        }
        while (rest.size() >= 8) {
            compress(load_le(rest.first<8>()));
            length_ += 8;
            rest = rest.subspan(8);
        }
        for (const auto byte : rest) {
            absorb_byte(byte);
        }
    }

    /** @brief The digest. Call once: it runs the finalization rounds on this state. */
    [[nodiscard]] constexpr std::uint64_t finish() noexcept {
        compress((static_cast<std::uint64_t>(length_ & 0xFF) << 56) | tail_);
        v2_ ^= 0xFF;
        for (int i{}; i < D; ++i) {
            round();
        }
        return v0_ ^ v1_ ^ v2_ ^ v3_;
    }

private:
    [[nodiscard]] static constexpr std::uint64_t load_le(const std::span<const std::byte, 8> bytes) noexcept {
        std::uint64_t word{};
        for (std::size_t i{}; i < 8; ++i) {
            word |= std::uint64_t{std::to_integer<std::uint8_t>(bytes[i])} << (8 * i);
        }
        return word;
    }

    constexpr void absorb_byte(const std::byte byte) noexcept {
        tail_ |= std::uint64_t{std::to_integer<std::uint8_t>(byte)} << (8 * (length_ & 7));
        if ((++length_ & 7) == 0) {
            compress(tail_);
            tail_ = 0;
        }
    }

    constexpr void compress(const std::uint64_t m) noexcept {
        v3_ ^= m;
        for (int i{}; i < C; ++i) {
            round();
        }
        v0_ ^= m;
    }

    constexpr void round() noexcept {
        v0_ += v1_;
        v1_ = std::rotl(v1_, 13);
        v1_ ^= v0_;
        v0_ = std::rotl(v0_, 32);
        v2_ += v3_;
        v3_ = std::rotl(v3_, 16);
        v3_ ^= v2_;
        v0_ += v3_;
        v3_ = std::rotl(v3_, 21);
        v3_ ^= v0_;
        v2_ += v1_;
        v1_ = std::rotl(v1_, 17);
        v1_ ^= v2_;
        v2_ = std::rotl(v2_, 32);
    }

    std::uint64_t v0_;
    std::uint64_t v1_;
    std::uint64_t v2_;
    std::uint64_t v3_;
    std::uint64_t tail_{};   ///< bytes since the last full word, little-endian
    std::uint64_t length_{}; ///< total bytes fed
};

/** @brief The variant the flow table uses. */
export using siphash = basic_siphash<1, 3>;

/** @brief Anything a key can feed its identity bytes to. */
export template <typename H>
concept byte_hasher = requires(H& h, const std::span<const std::byte> bytes) { h(bytes); };

/** @brief Feed an integer's object representation. */
export template <byte_hasher H, std::integral T> constexpr void hash_append(H& h, const T value) noexcept {
    const auto bytes{std::bit_cast<std::array<std::byte, sizeof(T)>>(value)};
    h(std::span<const std::byte>{bytes});
}

namespace detail {

/**
 * @brief One seed per process, for `std::hash`, which cannot carry its own.
 *
 * Zero if `getrandom` fails, which takes a kernel older than 3.17.
 */
[[nodiscard]] inline hash_seed process_seed() noexcept {
    static const hash_seed seed{random_seed().value_or(hash_seed{})};
    return seed;
}

} // namespace detail

} // namespace dgram
