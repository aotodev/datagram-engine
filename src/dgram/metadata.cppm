// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
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
     *
     * A v4 build accepts a v4-mapped address, which is what a dual-stack socket
     * reports for IPv4 arrivals, so a reply can reuse the `local_info` as is.
     */
    [[nodiscard]] static std::size_t build(::cmsghdr* dst, const value_type& value, const family fam) noexcept {
        if (fam == family::inet4) {
            ::in_pktinfo raw{};
            raw.ipi_ifindex = static_cast<int>(value.interface);
            if (value.address.is_v4() || value.address.is_v4_mapped()) {
                std::memcpy(&raw.ipi_spec_dst, value.address.address_bytes().last<sizeof(raw.ipi_spec_dst)>().data(), sizeof(raw.ipi_spec_dst));
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
 * Traffic class: DSCP and ECN
 * ============================================================================ */

/** @brief The two ECN bits of the traffic class, as RFC 3168 defines them. */
export enum class ecn_codepoint : std::uint8_t {
    not_ect = IPTOS_ECN_NOT_ECT, ///< not ECN-capable
    ect1 = IPTOS_ECN_ECT1,       ///< ECN-capable, codepoint 1
    ect0 = IPTOS_ECN_ECT0,       ///< ECN-capable, codepoint 0
    ce = IPTOS_ECN_CE,           ///< congestion experienced
};

/** @brief The six DSCP bits of the traffic class. Any value up to 63 is valid; these are the named ones. */
export enum class dscp : std::uint8_t {
    df = 0,           ///< default forwarding, best effort
    le = 1,           ///< lower effort, RFC 8622
    cs1 = 8,          ///< class selector 1
    af11 = 10,        ///< assured forwarding, class 1
    af12 = 12,        ///< assured forwarding, class 1
    af13 = 14,        ///< assured forwarding, class 1
    cs2 = 16,         ///< class selector 2
    af21 = 18,        ///< assured forwarding, class 2
    af22 = 20,        ///< assured forwarding, class 2
    af23 = 22,        ///< assured forwarding, class 2
    cs3 = 24,         ///< class selector 3
    af31 = 26,        ///< assured forwarding, class 3
    af32 = 28,        ///< assured forwarding, class 3
    af33 = 30,        ///< assured forwarding, class 3
    cs4 = 32,         ///< class selector 4
    af41 = 34,        ///< assured forwarding, class 4
    af42 = 36,        ///< assured forwarding, class 4
    af43 = 38,        ///< assured forwarding, class 4
    cs5 = 40,         ///< class selector 5
    voice_admit = 44, ///< capacity-admitted voice, RFC 5865
    ef = 46,          ///< expedited forwarding
    cs6 = 48,         ///< class selector 6, network control
    cs7 = 56,         ///< class selector 7
};

/** @brief The whole traffic-class byte: DSCP in the high six bits, ECN in the low two. */
export struct marking {
    dgram::dscp dscp{};
    ecn_codepoint ecn{};

    /** @brief The byte on the wire. A DSCP above 63 loses its high bits. */
    [[nodiscard]] constexpr std::uint8_t byte() const noexcept {
        return static_cast<std::uint8_t>(((std::to_underlying(dscp) & 0x3FU) << 2) | (std::to_underlying(ecn) & IPTOS_ECN_MASK));
    }

    [[nodiscard]] static constexpr marking from_byte(const std::uint8_t byte) noexcept {
        return {static_cast<dgram::dscp>(byte >> 2), static_cast<ecn_codepoint>(byte & IPTOS_ECN_MASK)};
    }

    // Not `= default`, for the GCC 16.2 crash noted on `hash_seed`.
    [[nodiscard]] friend constexpr bool operator==(const marking& a, const marking& b) noexcept { return a.byte() == b.byte(); }
};

namespace detail {

/**
 * @brief `IP_TOS` / `IPV6_TCLASS`: the one message behind `ecn` and `traffic_class`.
 *
 * The widths differ and the difference is not documented anywhere obvious:
 * `IP_TOS` arrives as a single byte, `IPV6_TCLASS` as a four-byte `int`. Reading
 * the wrong one overreads the control buffer, so `parse` branches on the length
 * the kernel actually wrote rather than on the family.
 */
struct tos_message {
    static constexpr std::size_t space{std::max(space_for(sizeof(std::uint8_t)), space_for(sizeof(int)))};

    [[nodiscard]] static bool matches(const int level, const int type) noexcept {
        return (level == IPPROTO_IP && type == IP_TOS) || (level == IPPROTO_IPV6 && type == IPV6_TCLASS);
    }

    [[nodiscard]] static std::optional<std::uint8_t> parse(const ::cmsghdr* c) noexcept {
        const auto width{payload_size(c)};
        if (width >= sizeof(int)) {
            return static_cast<std::uint8_t>(read_payload<int>(c));
        }
        if (width >= sizeof(std::uint8_t)) {
            return read_payload<std::uint8_t>(c);
        }
        return std::nullopt;
    }

    /**
     * @brief Written as an `int` for both families; the kernel accepts either width for `IP_TOS`.
     *
     * The kernel takes the value as the datagram's whole traffic-class byte,
     * replacing any DSCP the socket was given with `setsockopt`.
     */
    [[nodiscard]] static std::size_t build(::cmsghdr* dst, const std::uint8_t byte, const family fam) noexcept {
        const int value{byte};
        if (fam == family::inet4) {
            return write_message(dst, IPPROTO_IP, IP_TOS, value);
        }
        return write_message(dst, IPPROTO_IPV6, IPV6_TCLASS, value);
    }

    /**
     * @brief Ask the kernel to report the traffic class.
     *
     * A v6 socket also gets `IP_RECVTOS`: IPv4 traffic on a dual-stack socket
     * takes the kernel's IPv4 path, which reports `IP_TOS` and never
     * `IPV6_TCLASS`. The option is accepted on a v6-only socket too.
     */
    [[nodiscard]] static result<> enable(const int fd, const family fam) noexcept {
        const auto report_tos = [fd] { return set_option(fd, IPPROTO_IP, IP_RECVTOS, 1); };
        if (fam == family::inet4) {
            return report_tos();
        }
        return set_option(fd, IPPROTO_IPV6, IPV6_RECVTCLASS, 1) | then(report_tos);
    }
};

} // namespace detail

/**
 * @brief The ECN bits of a received datagram. Receive-only.
 *
 * Not sendable on purpose: the kernel takes a per-datagram traffic class as the
 * whole byte, so marking ECN alone would zero the DSCP. Send `traffic_class`.
 */
export struct ecn {
    using value_type = ecn_codepoint;

    static constexpr std::size_t space{detail::tos_message::space};

    [[nodiscard]] static bool matches(const int level, const int type) noexcept { return detail::tos_message::matches(level, type); }

    [[nodiscard]] static std::optional<value_type> parse(const ::cmsghdr* c) noexcept {
        return detail::tos_message::parse(c).transform([](const std::uint8_t byte) { return marking::from_byte(byte).ecn; });
    }

    [[nodiscard]] static result<> enable(const int fd, const family fam) noexcept { return detail::tos_message::enable(fd, fam); }
};

/** @brief The whole traffic class, DSCP and ECN, in both directions. */
export struct traffic_class {
    using value_type = marking;

    static constexpr std::size_t space{detail::tos_message::space};

    [[nodiscard]] static bool matches(const int level, const int type) noexcept { return detail::tos_message::matches(level, type); }

    [[nodiscard]] static std::optional<value_type> parse(const ::cmsghdr* c) noexcept { return detail::tos_message::parse(c).transform(marking::from_byte); }

    [[nodiscard]] static std::size_t build(::cmsghdr* dst, const value_type& value, const family fam) noexcept {
        return detail::tos_message::build(dst, value.byte(), fam);
    }

    [[nodiscard]] static result<> enable(const int fd, const family fam) noexcept { return detail::tos_message::enable(fd, fam); }
};

static_assert(cmsg_feature<pktinfo> && sendable_feature<pktinfo> && receivable_feature<pktinfo>);
static_assert(receivable_feature<ecn> && !sendable_feature<ecn>, "sending the ECN bits alone would zero the DSCP; send traffic_class");
static_assert(receivable_feature<traffic_class> && sendable_feature<traffic_class>);

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
