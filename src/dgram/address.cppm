/**
 * @file address.cppm
 * @brief IPv4 / IPv6 endpoints over `sockaddr_storage`.
 */
module;

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>

export module dgram:address;

import std;
import :error;

namespace dgram {

/** @brief Address family of an endpoint. */
export enum class family : std::uint8_t {
    inet4,
    inet6
};

/**
 * @brief An IP address and port, stored in the kernel's own layout.
 *
 * Holding `sockaddr_storage` rather than a tidier representation means the
 * receive path never converts: `recvmmsg` writes straight into the endpoint and
 * the send path hands the same bytes back.
 */
export class endpoint {
public:
    constexpr endpoint() noexcept : storage_{} { storage_.ss_family = AF_UNSPEC; }

    /** @brief Adopt a kernel-filled address. */
    explicit endpoint(const ::sockaddr_storage& raw) noexcept : storage_{raw} {}

    /** @brief Parse a textual address, `192.0.2.1` or `2001:db8::1`. */
    [[nodiscard]] static result<endpoint> parse(const family fam, const char* text, const std::uint16_t port) noexcept {
        endpoint ep{};
        if (fam == family::inet4) {
            auto& v4{reinterpret_cast<::sockaddr_in&>(ep.storage_)};
            v4.sin_family = AF_INET;
            v4.sin_port = ::htons(port);
            if (::inet_pton(AF_INET, text, &v4.sin_addr) != 1) {
                return std::unexpected{invalid_argument};
            }
        } else {
            auto& v6{reinterpret_cast<::sockaddr_in6&>(ep.storage_)};
            v6.sin6_family = AF_INET6;
            v6.sin6_port = ::htons(port);
            if (::inet_pton(AF_INET6, text, &v6.sin6_addr) != 1) {
                return std::unexpected{invalid_argument};
            }
        }
        return ep;
    }

    /** @brief The wildcard address for `fam`, for binding. */
    [[nodiscard]] static endpoint any(const family fam, const std::uint16_t port) noexcept {
        endpoint ep{};
        if (fam == family::inet4) {
            auto& v4{reinterpret_cast<::sockaddr_in&>(ep.storage_)};
            v4.sin_family = AF_INET;
            v4.sin_port = ::htons(port);
            v4.sin_addr.s_addr = ::htonl(INADDR_ANY);
        } else {
            auto& v6{reinterpret_cast<::sockaddr_in6&>(ep.storage_)};
            v6.sin6_family = AF_INET6;
            v6.sin6_port = ::htons(port);
            v6.sin6_addr = in6addr_any;
        }
        return ep;
    }

    [[nodiscard]] constexpr bool is_v4() const noexcept { return storage_.ss_family == AF_INET; }
    [[nodiscard]] constexpr bool is_v6() const noexcept { return storage_.ss_family == AF_INET6; }
    [[nodiscard]] constexpr bool valid() const noexcept { return is_v4() || is_v6(); }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return ::ntohs(is_v4() ? reinterpret_cast<const ::sockaddr_in&>(storage_).sin_port : reinterpret_cast<const ::sockaddr_in6&>(storage_).sin6_port);
    }

    /** @brief Bytes the kernel needs for this family, not `sizeof(storage)`. */
    [[nodiscard]] constexpr ::socklen_t size() const noexcept {
        return is_v4() ? static_cast<::socklen_t>(sizeof(::sockaddr_in)) : static_cast<::socklen_t>(sizeof(::sockaddr_in6));
    }

    [[nodiscard]] const ::sockaddr* raw() const noexcept { return reinterpret_cast<const ::sockaddr*>(&storage_); }

    /**
     * @brief The address itself: 4 bytes for v4, 16 for v6, empty if unspecified.
     *
     * The identity-bearing part. `sockaddr_storage` also carries `sin_zero`
     * padding and, for v6, a `sin6_flowinfo` the kernel may or may not populate;
     * neither identifies a peer, so neither belongs in a comparison or a hash.
     */
    [[nodiscard]] std::span<const std::byte> address_bytes() const noexcept {
        if (is_v4()) {
            const auto& v4{reinterpret_cast<const ::sockaddr_in&>(storage_)};
            return {reinterpret_cast<const std::byte*>(&v4.sin_addr), sizeof(v4.sin_addr)};
        }
        if (is_v6()) {
            const auto& v6{reinterpret_cast<const ::sockaddr_in6&>(storage_)};
            return {reinterpret_cast<const std::byte*>(&v6.sin6_addr), sizeof(v6.sin6_addr)};
        }
        return {};
    }

    /**
     * @brief IPv6 scope id, zero for v4.
     *
     * Part of the identity: `fe80::1%eth0` and `fe80::1%eth1` are different
     * destinations. `sin6_flowinfo`, which sits beside it, is not.
     */
    [[nodiscard]] std::uint32_t scope_id() const noexcept { return is_v6() ? reinterpret_cast<const ::sockaddr_in6&>(storage_).sin6_scope_id : 0U; }
    [[nodiscard]] ::sockaddr_storage& mutable_storage() noexcept { return storage_; }

    /** @brief Render as `addr:port`, or `[addr]:port` for IPv6. */
    [[nodiscard]] std::string text() const {
        std::array<char, INET6_ADDRSTRLEN> buf{};
        if (is_v4()) {
            const auto& v4{reinterpret_cast<const ::sockaddr_in&>(storage_)};
            ::inet_ntop(AF_INET, &v4.sin_addr, buf.data(), buf.size());
            return std::format("{}:{}", buf.data(), port());
        }
        const auto& v6{reinterpret_cast<const ::sockaddr_in6&>(storage_)};
        ::inet_ntop(AF_INET6, &v6.sin6_addr, buf.data(), buf.size());
        return std::format("[{}]:{}", buf.data(), port());
    }

    /**
     * @brief Equality over family, port, address and scope, and nothing else.
     *
     * Deliberately not a `memcmp` of the whole `sockaddr`: that would compare
     * `sin_zero` padding on v4 and `sin6_flowinfo` on v6. Flow labels are not
     * identity, and a peer whose flowinfo the kernel filled in would otherwise
     * look like a different peer on every datagram.
     */
    [[nodiscard]] friend bool operator==(const endpoint& a, const endpoint& b) noexcept {
        if (a.storage_.ss_family != b.storage_.ss_family || a.port() != b.port() || a.scope_id() != b.scope_id()) {
            return false;
        }
        return std::ranges::equal(a.address_bytes(), b.address_bytes());
    }

    /** @brief Hash over exactly the fields `operator==` compares. */
    [[nodiscard]] friend std::size_t hash_value(const endpoint& e) noexcept {
        // FNV-1a: no allocation, no table, and adequate for a demux key.
        std::uint64_t h{0xCBF29CE484222325ULL};
        const auto mix = [&h](const std::uint8_t byte) noexcept {
            h ^= byte;
            h *= 0x100000001B3ULL;
        };
        mix(static_cast<std::uint8_t>(e.storage_.ss_family));
        const auto port{e.port()};
        mix(static_cast<std::uint8_t>(port));
        mix(static_cast<std::uint8_t>(port >> 8));
        for (const auto byte : e.address_bytes()) {
            mix(static_cast<std::uint8_t>(byte));
        }
        const auto scope{e.scope_id()};
        for (int shift{}; shift < 32; shift += 8) {
            mix(static_cast<std::uint8_t>(scope >> shift));
        }
        return h;
    }

private:
    ::sockaddr_storage storage_;
};

static_assert(std::is_trivially_destructible_v<endpoint>);

} // namespace dgram

/** @brief So an `endpoint` can key a standard container as well as ours. */
template <> struct std::hash<dgram::endpoint> {
    [[nodiscard]] std::size_t operator()(const dgram::endpoint& e) const noexcept { return hash_value(e); }
};

namespace dgram {} // namespace dgram
