// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file pacing.cppm
 * @brief Transmit-time pacing: `SO_TXTIME`, `SCM_TXTIME`, and the rate arithmetic.
 *
 * Handing the kernel a departure time per datagram, instead of writing them as
 * fast as the socket accepts, is what keeps a sender from emitting micro-bursts
 * that overflow a router queue. Modern congestion control assumes it.
 */
module;

// Blank lines matter here: clang-format sorts within a contiguous block, so
// these stay separate blocks to keep <time.h> ahead of <linux/errqueue.h>,
// which declares a `struct timespec[3]` member and does not include it itself.
#include <cerrno>
#include <time.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <linux/errqueue.h>
#include <linux/net_tstamp.h>

export module dgram:pacing;

import std;

import :address;
import :cmsg;
import :error;
import :feature;

namespace dgram {

/**
 * @brief The clock a departure time is expressed against.
 *
 * `monotonic` is the default because it is the one an unprivileged process can
 * use: asking for `tai` without `CAP_NET_ADMIN` fails the socket option with
 * `EPERM`. `tai` is what you want when departure times are shared with other
 * machines, since it does not step.
 */
export enum class txtime_clock : int {
    monotonic = CLOCK_MONOTONIC,
    tai = CLOCK_TAI,
};

/** @brief `std::chrono::steady_clock` is `CLOCK_MONOTONIC` on this platform. */
export using pacing_clock = std::chrono::steady_clock;
static_assert(std::ratio_equal_v<pacing_clock::period, std::nano>, "the kernel wants nanoseconds");

/** @brief A departure time, as the kernel wants it: nanoseconds on the chosen clock. */
export using departure = std::chrono::nanoseconds;

/** @brief Now, on the clock `transmit_time` was configured with. */
export [[nodiscard]] inline departure now_on(const txtime_clock clock) noexcept {
    ::timespec ts{};
    ::clock_gettime(static_cast<::clockid_t>(clock), &ts);
    return std::chrono::seconds{ts.tv_sec} + std::chrono::nanoseconds{ts.tv_nsec};
}

/* ============================================================================
 * The socket option
 * ============================================================================ */

/**
 * @brief `SO_TXTIME`: accept a departure time per datagram.
 *
 * `Deadline` switches the kernel from "send at this time" to "send no later
 * than this time", which lets the qdisc reorder. `ReportErrors` routes missed
 * and malformed departures to the error queue, where `drain_transmit_error`
 * finds them; without it they are dropped silently. Only `etf` puts anything
 * there, so under `fq` this option is free and inert.
 *
 * @warning This option succeeding does **not** mean pacing works. The interface
 *          must carry the Fair Queue discipline:
 *          `tc qdisc add dev <iface> root fq`. Without it the kernel accepts
 *          every departure time and ignores every one of them, with no error
 *          anywhere. See docs/pacing.md.
 */
export template <txtime_clock Clock = txtime_clock::monotonic, bool Deadline = false, bool ReportErrors = true> struct transmit_time {
    [[nodiscard]] static result<> apply(const int fd, family) noexcept {
        ::sock_txtime config{};
        config.clockid = static_cast<::__kernel_clockid_t>(Clock);
        constexpr std::uint32_t deadline_bit{Deadline ? std::uint32_t{SOF_TXTIME_DEADLINE_MODE} : 0U};
        constexpr std::uint32_t report_bit{ReportErrors ? std::uint32_t{SOF_TXTIME_REPORT_ERRORS} : 0U};
        config.flags = deadline_bit | report_bit;
        if (::setsockopt(fd, SOL_SOCKET, SO_TXTIME, &config, static_cast<::socklen_t>(sizeof(config))) < 0) [[unlikely]] {
            return fail<>();
        }
        return {};
    }

    static constexpr txtime_clock clock{Clock};
};

/* ============================================================================
 * The control message
 * ============================================================================ */

/**
 * @brief `SCM_TXTIME`: when this datagram should leave.
 *
 * Send-only, per datagram, so one batch can mix paced and unpaced entries.
 *
 * Per datagram means per staged entry: a `segment`ed buffer is one entry, so
 * one departure covers all of its segments and pacing granularity becomes the
 * buffer rather than the datagram. See docs/pacing.md.
 *
 * The control message type and the socket option share a value
 * (`SCM_TXTIME == SO_TXTIME == 61`); that is the kernel's own aliasing, not a
 * mistake here.
 */
export struct txtime {
    using value_type = departure;

    static constexpr std::size_t space{detail::space_for(sizeof(std::uint64_t))};

    [[nodiscard]] static std::size_t build(::cmsghdr* dst, const value_type when, family) noexcept {
        const auto nanos{static_cast<std::uint64_t>(when.count())};
        return detail::write_message(dst, SOL_SOCKET, SCM_TXTIME, nanos);
    }
};

static_assert(cmsg_feature<txtime> && sendable_feature<txtime>);
static_assert(!parseable_feature<txtime>, "SCM_TXTIME is never received");

/* ============================================================================
 * Rate pacing
 * ============================================================================ */

/**
 * @brief Turns a byte rate into a departure time per datagram.
 *
 * Drift-free by construction: the sub-nanosecond remainder of each division is
 * carried into the next datagram rather than truncated, so a long run at a
 * fixed rate does not slowly fall behind. Accumulating rounded intervals is the
 * usual way this goes wrong, and at 1500-byte datagrams it loses about a second
 * per hour.
 *
 * Not thread-safe, like everything else here: one pacer per sending thread.
 */
export class pacer {
public:
    /** @brief Bytes per second. Zero means unpaced and yields the current time. */
    using rate_type = std::uint64_t;

    constexpr pacer(const rate_type bytes_per_second, const departure start) noexcept : rate_{bytes_per_second}, next_{start} {}

    /**
     * @brief The departure time for a datagram of `bytes`, advancing the schedule.
     *
     * @param bytes Wire size to charge against the rate. Charging payload only
     *              paces slightly fast; add the header overhead to be exact.
     */
    [[nodiscard]] constexpr departure schedule(const std::size_t bytes) noexcept {
        const auto at{next_};
        if (rate_ != 0) {
            next_ += interval_for(bytes);
        }
        return at;
    }

    /**
     * @brief Re-anchor the schedule to `now`, dropping any accumulated backlog.
     *
     * A sender that has been idle would otherwise have a schedule far in the
     * past and emit its next burst as fast as the socket accepts, which is the
     * behaviour pacing exists to prevent.
     */
    constexpr void resume_at(const departure now) noexcept {
        if (now > next_) {
            next_ = now;
            remainder_ = 0;
        }
    }

    /** @brief Change the rate without disturbing the current departure time. */
    constexpr void set_rate(const rate_type bytes_per_second) noexcept {
        rate_ = bytes_per_second;
        remainder_ = 0;
    }

    [[nodiscard]] constexpr rate_type rate() const noexcept { return rate_; }

    /** @brief The next departure time, without consuming it. */
    [[nodiscard]] constexpr departure peek() const noexcept { return next_; }

private:
    static constexpr std::uint64_t nanos_per_second{1'000'000'000};

    /** @brief `bytes / rate` seconds in nanoseconds, carrying the remainder. */
    [[nodiscard]] constexpr std::chrono::nanoseconds interval_for(const std::size_t bytes) noexcept {
        // bytes is bounded by a datagram, so this cannot overflow: the largest
        // numerator is 65535 * 1e9 plus a remainder below rate_.
        const auto numerator{static_cast<std::uint64_t>(bytes) * nanos_per_second + remainder_};
        remainder_ = numerator % rate_;
        return std::chrono::nanoseconds{static_cast<std::int64_t>(numerator / rate_)};
    }

    rate_type rate_;
    departure next_;
    std::uint64_t remainder_{};
};

/* ============================================================================
 * Error queue
 * ============================================================================ */

/** @brief Why the kernel rejected or missed a paced datagram. */
export enum class pacing_fault : std::uint8_t {
    invalid_departure, ///< the departure time was not usable
    missed_deadline,   ///< the datagram could not leave when it was due
    other,             ///< an error queue entry that is not a txtime one
};

/**
 * @brief Drain one entry from the error queue, if any.
 *
 * Only populated when `transmit_time` was configured with `ReportErrors`, and
 * then only under a discipline that reports: **`fq` never does.** It honours a
 * departure time and drops what it cannot queue, including anything past its
 * `horizon`, without a word anywhere. `etf` is the discipline that answers here,
 * at the price of insisting its clock matches the socket's and wanting
 * `CLOCK_TAI`, which needs `CAP_NET_ADMIN`.
 *
 * An empty queue under `fq` is therefore no information at all, and in
 * particular is not evidence that pacing is working. See docs/pacing.md.
 *
 * @return The fault, or nothing when the queue is empty. `would_block` is not an
 *         error here and is reported as an empty queue.
 */
export [[nodiscard]] inline result<std::optional<pacing_fault>> drain_transmit_error(const int fd) noexcept {
    std::array<std::byte, 256> payload{};
    alignas(::cmsghdr) std::array<std::byte, 512> control{};

    ::iovec io{payload.data(), payload.size()};
    ::msghdr m{};
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = control.data();
    m.msg_controllen = control.size();

    if (::recvmsg(fd, &m, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::optional<pacing_fault>{};
        }
        return fail<std::optional<pacing_fault>>();
    }

    for (const ::cmsghdr* c{detail::first_header(m)}; c != nullptr; c = detail::next_header(m, c)) {
        if (!detail::within_buffer(m, c) || detail::payload_size(c) < sizeof(::sock_extended_err)) {
            break;
        }
        const bool is_error{(c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) || (c->cmsg_level == SOL_IPV6 && c->cmsg_type == IPV6_RECVERR)};
        if (!is_error) {
            continue;
        }
        const auto err{detail::read_payload<::sock_extended_err>(c)};
        if (err.ee_origin != SO_EE_ORIGIN_TXTIME) {
            return std::optional{pacing_fault::other};
        }
        switch (err.ee_code) {
        case SO_EE_CODE_TXTIME_INVALID_PARAM:
            return std::optional{pacing_fault::invalid_departure};
        case SO_EE_CODE_TXTIME_MISSED:
            return std::optional{pacing_fault::missed_deadline};
        default:
            return std::optional{pacing_fault::other};
        }
    }
    return std::optional{pacing_fault::other};
}

} // namespace dgram
