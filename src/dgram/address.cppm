/**
 * @file address.cppm
 * @brief IPv4 / IPv6 endpoints over `sockaddr_storage`.
 */
module;

#include <cerrno>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

export module dgram:address;

import std;
import :error;

namespace dgram {

/** @brief Address family of an endpoint. */
export enum class family : std::uint8_t { inet4, inet6 };

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

    /** @brief Byte-wise equality over the significant prefix only. */
    [[nodiscard]] friend bool operator==(const endpoint& a, const endpoint& b) noexcept {
        if (a.storage_.ss_family != b.storage_.ss_family) {
            return false;
        }
        return std::memcmp(&a.storage_, &b.storage_, a.size()) == 0;
    }

private:
    ::sockaddr_storage storage_;
};

static_assert(std::is_trivially_destructible_v<endpoint>);

} // namespace dgram
