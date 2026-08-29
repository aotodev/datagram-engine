/**
 * @file metadata.cppm
 * @brief The ancillary-data features: destination address and ECN.
 */
module;

#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>

export module dgram:metadata;

import std;

import :address;
import :cmsg;
import :error;
import :feature;
import :socket;

namespace dgram {

/* ============================================================================
 * Destination address
 * ============================================================================ */

/**
 * @brief Which local address and interface a datagram arrived on.
 *
 * The reason a wildcard-bound socket can still answer from the address the peer
 * addressed, which matters the moment a host has more than one.
 */
export struct local_info {
    endpoint address{};       ///< the local address the datagram was sent to
    unsigned int interface{}; ///< interface index, `0` when unspecified
};

/**
 * @brief `IP_PKTINFO` / `IPV6_PKTINFO`.
 *
 * Sized for the larger of the two payloads, because the receive path cannot know
 * which family will arrive: a dual-stack socket reports `IPV6_PKTINFO` even for
 * v4-mapped traffic.
 */
export struct pktinfo {
    using value_type = local_info;

    static constexpr std::size_t space{std::max(detail::space_for(sizeof(::in_pktinfo)), detail::space_for(sizeof(::in6_pktinfo)))};

    [[nodiscard]] static bool matches(const int level, const int type) noexcept {
        return (level == IPPROTO_IP && type == IP_PKTINFO) || (level == IPPROTO_IPV6 && type == IPV6_PKTINFO);
    }

    [[nodiscard]] static std::optional<value_type> parse(const ::cmsghdr* c) noexcept {
        if (c->cmsg_level == IPPROTO_IP) {
            if (detail::payload_size(c) < sizeof(::in_pktinfo)) {
                return std::nullopt;
            }
            const auto raw{detail::read_payload<::in_pktinfo>(c)};
            ::sockaddr_storage storage{};
            auto& v4{reinterpret_cast<::sockaddr_in&>(storage)};
            v4.sin_family = AF_INET;
            v4.sin_addr = raw.ipi_addr;
            return value_type{endpoint{storage}, static_cast<unsigned int>(raw.ipi_ifindex)};
        }
        if (detail::payload_size(c) < sizeof(::in6_pktinfo)) {
            return std::nullopt;
        }
        const auto raw{detail::read_payload<::in6_pktinfo>(c)};
        ::sockaddr_storage storage{};
        auto& v6{reinterpret_cast<::sockaddr_in6&>(storage)};
        v6.sin6_family = AF_INET6;
        v6.sin6_addr = raw.ipi6_addr;
        return value_type{endpoint{storage}, static_cast<unsigned int>(raw.ipi6_ifindex)};
    }

    /**
     * @brief Pick the source address and interface for an outgoing datagram.
     *
     * Only the address and interface are set; leaving both zero lets the routing
     * table choose, which is the same thing as not attaching the header at all.
     */
    [[nodiscard]] static std::size_t build(::cmsghdr* dst, const value_type& value, const family fam) noexcept {
        if (fam == family::inet4) {
            ::in_pktinfo raw{};
            raw.ipi_ifindex = static_cast<int>(value.interface);
            if (value.address.is_v4()) {
                raw.ipi_spec_dst = reinterpret_cast<const ::sockaddr_in*>(value.address.raw())->sin_addr;
            }
            return detail::write_message(dst, IPPROTO_IP, IP_PKTINFO, raw);
        }
        ::in6_pktinfo raw{};
        raw.ipi6_ifindex = value.interface;
        if (value.address.is_v6()) {
            raw.ipi6_addr = reinterpret_cast<const ::sockaddr_in6*>(value.address.raw())->sin6_addr;
        }
        return detail::write_message(dst, IPPROTO_IPV6, IPV6_PKTINFO, raw);
    }

    /** @brief Ask the kernel to report the destination address. */
    [[nodiscard]] static result<> enable(const int fd, const family fam) noexcept {
        const int on{1};
        const int level{fam == family::inet4 ? IPPROTO_IP : IPPROTO_IPV6};
        const int name{fam == family::inet4 ? IP_PKTINFO : IPV6_RECVPKTINFO};
        if (::setsockopt(fd, level, name, &on, static_cast<::socklen_t>(sizeof(on))) < 0) [[unlikely]] {
            return fail<>();
        }
        return {};
    }
};

/* ============================================================================
 * Explicit Congestion Notification
 * ============================================================================ */

/** @brief The two ECN bits of the traffic class, as RFC 3168 defines them. */
export enum class ecn_codepoint : std::uint8_t {
    not_ect = IPTOS_ECN_NOT_ECT, ///< not ECN-capable
    ect1 = IPTOS_ECN_ECT1,       ///< ECN-capable, codepoint 1
    ect0 = IPTOS_ECN_ECT0,       ///< ECN-capable, codepoint 0
    ce = IPTOS_ECN_CE,           ///< congestion experienced
};

/**
 * @brief `IP_TOS` / `IPV6_TCLASS`, reduced to the ECN bits.
 *
 * The widths differ and the difference is not documented anywhere obvious:
 * `IP_TOS` arrives as a single byte, `IPV6_TCLASS` as a four-byte `int`. Reading
 * the wrong one overreads the control buffer, so `parse` branches on the length
 * the kernel actually wrote rather than on the family.
 */
export struct ecn {
    using value_type = ecn_codepoint;

    static constexpr std::size_t space{std::max(detail::space_for(sizeof(std::uint8_t)), detail::space_for(sizeof(int)))};

    [[nodiscard]] static bool matches(const int level, const int type) noexcept {
        return (level == IPPROTO_IP && type == IP_TOS) || (level == IPPROTO_IPV6 && type == IPV6_TCLASS);
    }

    [[nodiscard]] static std::optional<value_type> parse(const ::cmsghdr* c) noexcept {
        const auto width{detail::payload_size(c)};
        if (width >= sizeof(int)) {
            return from_traffic_class(detail::read_payload<int>(c));
        }
        if (width >= sizeof(std::uint8_t)) {
            return from_traffic_class(detail::read_payload<std::uint8_t>(c));
        }
        return std::nullopt;
    }

    /**
     * @brief Mark an outgoing datagram.
     *
     * Written at the width each family expects, mirroring the receive side.
     */
    [[nodiscard]] static std::size_t build(::cmsghdr* dst, const value_type value, const family fam) noexcept {
        const int bits{static_cast<int>(std::to_underlying(value))};
        if (fam == family::inet4) {
            return detail::write_message(dst, IPPROTO_IP, IP_TOS, bits);
        }
        return detail::write_message(dst, IPPROTO_IPV6, IPV6_TCLASS, bits);
    }

    /** @brief Ask the kernel to report the traffic class. */
    [[nodiscard]] static result<> enable(const int fd, const family fam) noexcept {
        const int on{1};
        const int level{fam == family::inet4 ? IPPROTO_IP : IPPROTO_IPV6};
        const int name{fam == family::inet4 ? IP_RECVTOS : IPV6_RECVTCLASS};
        if (::setsockopt(fd, level, name, &on, static_cast<::socklen_t>(sizeof(on))) < 0) [[unlikely]] {
            return fail<>();
        }
        return {};
    }

private:
    [[nodiscard]] static constexpr ecn_codepoint from_traffic_class(const std::integral auto tclass) noexcept {
        return static_cast<ecn_codepoint>(static_cast<std::uint8_t>(tclass) & IPTOS_ECN_MASK);
    }
};

static_assert(cmsg_feature<pktinfo> && sendable_feature<pktinfo> && receivable_feature<pktinfo>);
static_assert(cmsg_feature<ecn> && sendable_feature<ecn> && receivable_feature<ecn>);

/* ============================================================================
 * Enabling a whole set at open time
 * ============================================================================ */

/**
 * @brief Socket option enabling every feature in the set for receive.
 *
 * The set the socket reports and the set the batch is sized for should be the
 * same one, so write it once and pass it to both.
 */
export template <receivable_feature... Fs> struct receive_metadata {
    [[nodiscard]] static result<> apply(const int fd, const family fam) noexcept {
        result<> applied{};
        (void)((applied = Fs::enable(fd, fam), applied.has_value()) && ...);
        return applied;
    }
};

} // namespace dgram
