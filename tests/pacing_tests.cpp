/**
 * @file pacing_tests.cpp
 * @brief Transmit-time pacing.
 *
 * The rate arithmetic is tested exhaustively because it is the part that works
 * identically everywhere. The kernel side is only partly reachable here: real
 * pacing needs `tc qdisc add dev <iface> root fq`, which needs CAP_NET_ADMIN,
 * so what is asserted below is that the option and the control message are
 * accepted and that nothing is corrupted, not that a datagram was delayed.
 */
#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>

import std;
import libmem;
import dgram;

namespace {

using namespace std::chrono_literals;

constexpr std::size_t capacity{8};
constexpr std::size_t slot{2048};
constexpr dgram::pacer::rate_type gigabit{125'000'000}; // bytes/s

using tx_set = dgram::features<dgram::txtime, dgram::ecn>;
using tx_batch = dgram::transmit_batch<capacity, slot, tx_set>;
using rx_batch = dgram::receive_batch<capacity, slot>;

std::span<const std::byte> bytes_of(std::string_view s) noexcept {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

/* ============================================================================
 * Rate arithmetic
 * ============================================================================ */

TEST(Pacer, FirstDatagramLeavesImmediately) {
    dgram::pacer p{gigabit, 1000ns};
    EXPECT_EQ(p.schedule(1500), 1000ns) << "the schedule starts where it was anchored";
}

TEST(Pacer, IntervalMatchesTheRate) {
    dgram::pacer p{1'000'000, 0ns}; // 1 MB/s: 1000 bytes takes exactly 1 ms
    EXPECT_EQ(p.schedule(1000), 0ns);
    EXPECT_EQ(p.schedule(1000), 1ms);
    EXPECT_EQ(p.schedule(1000), 2ms);
}

TEST(Pacer, RateZeroIsUnpaced) {
    dgram::pacer p{0, 500ns};
    for (int i{}; i < 10; ++i) {
        EXPECT_EQ(p.schedule(1500), 500ns) << "an unpaced schedule never advances";
    }
}

TEST(Pacer, DepartureTimesAreMonotonic) {
    dgram::pacer p{gigabit, 0ns};
    auto previous{p.schedule(1)};
    for (std::size_t i{}; i < 10'000; ++i) {
        const auto next{p.schedule(1 + (i % 1500))};
        EXPECT_GE(next, previous);
        previous = next;
    }
}

/* Accumulating rounded intervals is the usual way this goes wrong: at a rate
   where the per-datagram interval is not a whole number of nanoseconds, a naive
   pacer loses the remainder every time and drifts. */
TEST(Pacer, DoesNotDriftOverManyDatagrams) {
    // The rate must not divide `size * 1e9` evenly, or there is no remainder to
    // lose and the test passes against a truncating pacer too. 1500 * 1e9 / 7e6
    // is 214285.714..., so every datagram sheds a fraction of a nanosecond.
    constexpr dgram::pacer::rate_type awkward{7'000'000};
    constexpr std::size_t datagrams{100'000};
    constexpr std::size_t size{1500};
    static_assert(static_cast<std::uint64_t>(size) * 1'000'000'000U % awkward != 0, "the rate has to leave a remainder");

    dgram::pacer p{awkward, 0ns};
    for (std::size_t i{}; i < datagrams; ++i) {
        (void)p.schedule(size);
    }

    // Exact answer: total_bytes * 1e9 / rate, computed without intermediate rounding.
    const auto total_bytes{static_cast<__uint128_t>(datagrams) * size};
    const auto exact_ns{static_cast<std::int64_t>(total_bytes * 1'000'000'000U / awkward)};

    EXPECT_EQ(p.peek().count(), exact_ns) << "the remainder must be carried, not truncated";
}

TEST(Pacer, DriftFreeAcrossVariableSizes) {
    constexpr dgram::pacer::rate_type rate{7'777'777};
    dgram::pacer p{rate, 0ns};
    __uint128_t total{};
    for (std::size_t i{1}; i <= 20'000; ++i) {
        const std::size_t size{1 + (i * 37) % 1500};
        total += size;
        (void)p.schedule(size);
    }
    const auto exact_ns{static_cast<std::int64_t>(total * 1'000'000'000U / rate)};
    EXPECT_EQ(p.peek().count(), exact_ns);
}

TEST(Pacer, ResumeDropsBacklogButNeverRewinds) {
    dgram::pacer p{1'000'000, 0ns};
    for (int i{}; i < 100; ++i) {
        (void)p.schedule(1000); // schedule now 100 ms ahead
    }
    ASSERT_EQ(p.peek(), 100ms);

    // A sender idle until 500 ms must not then burst to catch up.
    p.resume_at(500ms);
    EXPECT_EQ(p.peek(), 500ms);

    // Resuming at a time already passed must not rewind the schedule.
    p.resume_at(10ms);
    EXPECT_EQ(p.peek(), 500ms) << "resume_at only ever moves forward";
}

TEST(Pacer, ExtremeRatesStaySane) {
    // One byte per second: a 1500-byte datagram takes 1500 seconds.
    dgram::pacer slow{1, 0ns};
    (void)slow.schedule(1500);
    EXPECT_EQ(slow.peek(), 1500s);

    // A rate far above any link: the interval collapses to zero but stays ordered.
    dgram::pacer fast{std::numeric_limits<dgram::pacer::rate_type>::max(), 0ns};
    const auto a{fast.schedule(1500)};
    const auto b{fast.schedule(1500)};
    EXPECT_GE(b, a);
    EXPECT_GE(fast.peek(), 0ns);
}

TEST(Pacer, SetRateKeepsTheCurrentDeparture) {
    dgram::pacer p{1'000'000, 0ns};
    (void)p.schedule(1000);
    const auto before{p.peek()};
    p.set_rate(2'000'000);
    EXPECT_EQ(p.peek(), before) << "changing rate must not move an already-scheduled departure";
    EXPECT_EQ(p.rate(), 2'000'000u);
    (void)p.schedule(1000);
    EXPECT_EQ(p.peek(), before + 500us) << "the new rate applies from here on";
}

TEST(Pacer, IsConstexpr) {
    constexpr auto total = [] {
        dgram::pacer p{1'000'000, 0ns};
        (void)p.schedule(1000);
        (void)p.schedule(1000);
        return p.peek();
    }();
    static_assert(total == 2ms);
    SUCCEED();
}

/* ============================================================================
 * The kernel side
 * ============================================================================ */

TEST(TransmitTime, MonotonicClockIsAcceptedUnprivileged) {
    const auto sock{dgram::socket::open<dgram::transmit_time<>>(dgram::family::inet4)};
    ASSERT_TRUE(sock.has_value()) << dgram::describe(sock.error());
}

/* CLOCK_TAI needs CAP_NET_ADMIN, which is why monotonic is the default. The
   failure must be reported rather than silently downgraded. */
TEST(TransmitTime, TaiClockNeedsPrivilegeAndSaysSo) {
    const auto sock{dgram::socket::open<dgram::transmit_time<dgram::txtime_clock::tai>>(dgram::family::inet4)};
    if (!sock.has_value()) {
        EXPECT_EQ(sock.error(), dgram::errc{EPERM}) << "the only expected failure is a privilege one";
    } else {
        SUCCEED() << "running privileged, CLOCK_TAI accepted";
    }
}

TEST(TransmitTime, PacedDatagramStillArrives) {
    libmem::arena arena{rx_batch::footprint() + tx_batch::footprint()};

    auto receiver{dgram::socket::open<dgram::reuse_addr>(dgram::family::inet4)};
    auto sender{dgram::socket::open<dgram::transmit_time<>>(dgram::family::inet4)};
    ASSERT_TRUE(receiver.has_value() && sender.has_value());
    ASSERT_TRUE(receiver->bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());
    const auto target{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", receiver->local_address()->port())};

    auto rx{rx_batch::carve(arena)};
    auto tx{tx_batch::carve(arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    dgram::pacer p{gigabit, dgram::now_on(dgram::txtime_clock::monotonic)};
    dgram::control<tx_set> ancillary{};
    ancillary.set<dgram::txtime>(p.schedule(64));

    ASSERT_TRUE(tx->stage_copy(bytes_of("paced"), target, ancillary));
    const auto sent{tx->flush(*sender)};
    ASSERT_TRUE(sent.has_value()) << dgram::describe(sent.error());
    EXPECT_EQ(*sent, 1u);

    // Without fq on the interface the kernel ignores the departure time and
    // sends immediately, so this arrives either way. What is asserted is that
    // attaching SCM_TXTIME does not corrupt or drop the datagram.
    ASSERT_EQ(*rx->receive(*receiver), 1u);
    const auto view{rx->datagrams()};
    const auto d{*view.begin()};
    EXPECT_TRUE(d.intact());
    EXPECT_EQ(d.payload().size(), std::string_view{"paced"}.size());
}

TEST(TransmitTime, PacingAndEcnTravelTogether) {
    libmem::arena arena{tx_batch::footprint()};
    auto tx{tx_batch::carve(arena)};
    ASSERT_TRUE(tx.has_value());

    dgram::control<tx_set> ancillary{};
    ancillary.set<dgram::txtime>(1'000'000ns);
    ancillary.set<dgram::ecn>(dgram::ecn_codepoint::ect0);

    const auto target{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", 9)};
    EXPECT_TRUE(tx->stage(bytes_of("x"), target, ancillary));
    EXPECT_EQ(tx->staged(), 1u);
}

/* Nothing is queued because nothing was paced; the drain must report an empty
   queue rather than an error. */
TEST(TransmitTime, ErrorQueueIsEmptyWhenNothingWentWrong) {
    const auto sock{dgram::socket::open<dgram::transmit_time<>>(dgram::family::inet4)};
    ASSERT_TRUE(sock.has_value());
    const auto fault{dgram::drain_transmit_error(sock->native())};
    ASSERT_TRUE(fault.has_value()) << "an empty error queue is not a failure";
    EXPECT_FALSE(fault->has_value());
}

TEST(TransmitTime, NowOnReturnsAdvancingTime) {
    const auto a{dgram::now_on(dgram::txtime_clock::monotonic)};
    const auto b{dgram::now_on(dgram::txtime_clock::monotonic)};
    EXPECT_GE(b, a);
    EXPECT_GT(a.count(), 0);
}

} // namespace
