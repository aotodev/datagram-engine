/**
 * @file offload.cppm
 * @brief Hardware segmentation offload: `UDP_GRO` on receive, `UDP_SEGMENT` on send.
 *
 * The point of both is to move the per-datagram framing loop out of user space.
 * One `recvmmsg` slot can come back holding many datagrams' worth of bytes, and
 * one `sendmmsg` entry can carry a buffer the kernel or the NIC then slices.
 */
module;

#include <cerrno>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <sys/socket.h>

export module dgram:offload;

import std;

import :address;
import :cmsg;
import :error;
import :feature;

namespace dgram {

/**
 * @brief `UDP_GRO`: receive several datagrams coalesced into one buffer.
 *
 * The value is the size every datagram in the buffer had except possibly the
 * last, which is whatever remained. Receive-only; there is no such thing as
 * sending a `UDP_GRO` control message.
 *
 * The kernel omits the control message entirely when it did not coalesce
 * anything, so absent means "this buffer holds exactly one datagram" rather than
 * "reporting was off".
 */
export struct gro {
    /* The kernel writes an int, though the value is bounded by USHRT_MAX. */
    using value_type = std::uint16_t;

    static constexpr std::size_t space{detail::space_for(sizeof(int))};

    /**
     * @brief Largest buffer the kernel will coalesce into one slot.
     *
     * The number a receive slot has to be sized for once this feature is in the
     * set. It is not asserted here because an undersized slot is a legitimate
     * thing to build, and to test: truncation is reported rather than hidden,
     * and this engine's own suite constructs the case on purpose. A consumer
     * that always enables coalescing should assert against this rather than
     * against a literal, so the number stays in one place.
     */
    static constexpr std::size_t max_coalesced{1U << 16};

    [[nodiscard]] static bool matches(const int level, const int type) noexcept { return level == SOL_UDP && type == UDP_GRO; }

    [[nodiscard]] static std::optional<value_type> parse(const ::cmsghdr* c) noexcept {
        if (detail::payload_size(c) < sizeof(int)) {
            return std::nullopt;
        }
        const auto size{detail::read_payload<int>(c)};
        if (size <= 0 || size > std::numeric_limits<value_type>::max()) {
            return std::nullopt;
        }
        return static_cast<value_type>(size);
    }

    /**
     * @brief Ask the kernel to coalesce.
     *
     * A GRO-enabled socket can hand back far more than one datagram per slot, so
     * `slot_bytes` has to be sized for the coalesced buffer rather than for one
     * MTU. Leaving it at an MTU turns coalescing into constant `MSG_TRUNC`.
     */
    [[nodiscard]] static result<> enable(const int fd, family) noexcept {
        const int on{1};
        if (::setsockopt(fd, IPPROTO_UDP, UDP_GRO, &on, static_cast<::socklen_t>(sizeof(on))) < 0) [[unlikely]] {
            return fail<>();
        }
        return {};
    }
};

/**
 * @brief `UDP_SEGMENT`: hand the kernel one large buffer and a slice size.
 *
 * Send-only, and per datagram rather than per socket, so one batch can mix
 * segmented and ordinary entries. The final slice is whatever remains and may be
 * shorter than the rest.
 *
 * The payload is a `uint16_t`, not the `int` that `UDP_GRO` reports.
 */
export struct segment {
    using value_type = std::uint16_t;

    static constexpr std::size_t space{detail::space_for(sizeof(value_type))};

    [[nodiscard]] static std::size_t build(::cmsghdr* dst, const value_type size, family) noexcept {
        return detail::write_message(dst, SOL_UDP, UDP_SEGMENT, size);
    }
};

static_assert(cmsg_feature<gro> && parseable_feature<gro> && receivable_feature<gro>);
static_assert(cmsg_feature<segment> && sendable_feature<segment>);
static_assert(!sendable_feature<gro>, "there is no such thing as sending a UDP_GRO message");
static_assert(!parseable_feature<segment>, "UDP_SEGMENT is never received");

/* ============================================================================
 * Walking a coalesced buffer
 * ============================================================================ */

namespace detail {

/** @brief Slices `bytes` at `stride`, last slice short. */
struct segmenter {
    std::span<const std::byte> bytes;
    std::size_t stride;

    [[nodiscard]] constexpr std::span<const std::byte> operator()(const std::size_t index) const noexcept {
        const auto offset{index * stride};
        return bytes.subspan(offset, std::min(stride, bytes.size() - offset));
    }
};

/**
 * @brief How many datagrams `total` bytes hold at `stride` each.
 *
 * One, not zero, when there are no bytes: a zero-length UDP datagram is legal
 * and dropping it here would lose it silently.
 */
[[nodiscard]] constexpr std::size_t segment_count(const std::size_t total, const std::size_t stride) noexcept {
    if (total == 0 || stride == 0) {
        return 1;
    }
    return (total + stride - 1) / stride;
}

} // namespace detail

/**
 * @brief Lazy view over the datagrams packed into one coalesced buffer.
 *
 * A view rather than a generator on purpose: this is the innermost loop of the
 * receive path, and `std::generator` would put a heap allocation on it.
 *
 * `stride` of zero, or a stride at least as large as the buffer, yields the
 * whole buffer as a single datagram, which is exactly the no-coalescing case.
 */
export [[nodiscard]] constexpr auto segments_of(const std::span<const std::byte> bytes, const std::size_t stride) noexcept {
    const auto effective{stride == 0 ? bytes.size() : stride};
    return std::views::iota(std::size_t{0}, detail::segment_count(bytes.size(), effective)) | std::views::transform(detail::segmenter{bytes, effective});
}

} // namespace dgram
