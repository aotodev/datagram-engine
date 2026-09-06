// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

import std;
import libmem;
import dgram;

namespace {

constexpr std::size_t capacity{16};
constexpr std::size_t slot{512};

using rx_batch = dgram::receive_batch<capacity, slot>;
using tx_batch = dgram::transmit_batch<capacity, slot>;

std::span<const std::byte> bytes_of(std::string_view s) noexcept {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

/** A bound receiver and a sender aimed at it, over v4 loopback. */
struct loopback {
    libmem::arena arena{rx_batch::footprint() + tx_batch::footprint()};
    dgram::socket receiver{std::move(*dgram::socket::open<dgram::reuse_addr>(dgram::family::inet4))};
    dgram::socket sender{std::move(*dgram::socket::open<>(dgram::family::inet4))};
    dgram::endpoint target{};

    loopback() {
        EXPECT_TRUE(receiver.bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());
        auto local{receiver.local_address()};
        EXPECT_TRUE(local.has_value());
        target = *dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", local->port());
    }
};

TEST(Layout, FootprintCoversAnActualCarve) {
    libmem::arena arena{rx_batch::footprint()};
    const auto batch{rx_batch::carve(arena)};
    ASSERT_TRUE(batch.has_value());
    EXPECT_LE(arena.used(), rx_batch::footprint());
}

TEST(Layout, UndersizedArenaFailsCleanly) {
    libmem::arena arena{64};
    const auto batch{rx_batch::carve(arena)};
    ASSERT_FALSE(batch.has_value());
    EXPECT_EQ(batch.error(), dgram::errc{ENOMEM});
}

TEST(Endpoint, RoundTripsTextAndPort) {
    const auto ep{dgram::endpoint::parse(dgram::family::inet4, "192.0.2.7", 4433)};
    ASSERT_TRUE(ep.has_value());
    EXPECT_TRUE(ep->is_v4());
    EXPECT_EQ(ep->port(), 4433);
    EXPECT_EQ(ep->text(), "192.0.2.7:4433");

    const auto v6{dgram::endpoint::parse(dgram::family::inet6, "2001:db8::1", 443)};
    ASSERT_TRUE(v6.has_value());
    EXPECT_TRUE(v6->is_v6());
    EXPECT_EQ(v6->text(), "[2001:db8::1]:443");
    EXPECT_NE(*ep, *v6);
}

TEST(Endpoint, RejectsMalformedText) {
    EXPECT_FALSE(dgram::endpoint::parse(dgram::family::inet4, "not-an-address", 1).has_value());
}

TEST(Loopback, DeliversAWholeBatch) {
    loopback net{};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value());
    ASSERT_TRUE(tx.has_value());

    constexpr std::size_t sent_count{8};
    std::array<std::string, sent_count> payloads{};
    for (auto [i, p] : std::views::enumerate(payloads)) {
        p = std::format("datagram-{}", i);
        ASSERT_TRUE(tx->stage_copy(bytes_of(p), net.target));
    }
    const auto sent{tx->flush(net.sender)};
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(*sent, sent_count);

    const auto got{rx->receive(net.receiver)};
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, sent_count);

    const auto texts{rx->datagrams() | std::views::transform([](const auto& d) {
        return std::string{reinterpret_cast<const char*>(d.payload().data()), d.payload().size()};
    }) | std::ranges::to<std::vector>()};
    EXPECT_TRUE(std::ranges::equal(texts, payloads));
    EXPECT_TRUE(std::ranges::all_of(rx->datagrams(), dgram::is_intact));
}

// The trap this whole design exists around: reusing a wired batch must not
// shrink msg_namelen or msg_controllen from the previous call's actuals.
TEST(Loopback, SurvivesRepeatedReceivesOnTheSameBatch) {
    loopback net{};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value());
    ASSERT_TRUE(tx.has_value());

    for (int round{}; round < 4; ++round) {
        const auto text{std::format("round-{}", round)};
        ASSERT_TRUE(tx->stage_copy(bytes_of(text), net.target));
        ASSERT_TRUE(tx->flush(net.sender).has_value());

        const auto got{rx->receive(net.receiver)};
        ASSERT_TRUE(got.has_value());
        ASSERT_EQ(*got, 1u);

        const auto view{rx->datagrams()};
        const auto d{*view.begin()};
        EXPECT_EQ(d.payload().size(), text.size());
        EXPECT_TRUE(d.intact()) << "round " << round;
        EXPECT_TRUE(d.from().is_v4());
        EXPECT_EQ(d.from().port(), net.sender.local_address()->port());
    }
}

TEST(Loopback, ReportsTruncationRatherThanCorrupting) {
    using tiny_rx = dgram::receive_batch<4, 8>;

    loopback net{};
    auto rx{tiny_rx::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value());
    ASSERT_TRUE(tx.has_value());

    ASSERT_TRUE(tx->stage_copy(bytes_of("far longer than eight bytes"), net.target));
    ASSERT_TRUE(tx->flush(net.sender).has_value());

    const auto got{rx->receive(net.receiver)};
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(*got, 1u);

    const auto view{rx->datagrams()};
    const auto d{*view.begin()};
    EXPECT_TRUE(d.truncated()) << "an oversized datagram must not look intact";
    EXPECT_FALSE(d.intact());
    EXPECT_EQ(d.payload().size(), 8u);
}

// A reactor drives the engine nonblocking, where EAGAIN-then-data is the steady
// state. This pins the rearm invariant across a failed receive: recvmmsg must not
// have clobbered the headers on the way out.
TEST(Loopback, NonblockingReceiveReportsWouldBlockThenRecovers) {
    libmem::arena arena{rx_batch::footprint() + tx_batch::footprint()};
    dgram::socket receiver{std::move(*dgram::socket::open<dgram::reuse_addr, dgram::nonblocking>(dgram::family::inet4))};
    dgram::socket sender{std::move(*dgram::socket::open<>(dgram::family::inet4))};

    ASSERT_TRUE(receiver.bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());
    const auto target{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", receiver.local_address()->port())};

    auto rx{rx_batch::carve(arena)};
    auto tx{tx_batch::carve(arena)};
    ASSERT_TRUE(rx.has_value());
    ASSERT_TRUE(tx.has_value());

    const auto empty{rx->receive(receiver)};
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error(), dgram::would_block);
    EXPECT_EQ(rx->received(), 0u);

    constexpr std::string_view text{"after the empty poll"};
    ASSERT_TRUE(tx->stage_copy(bytes_of(text), target));
    ASSERT_TRUE(tx->flush(sender).has_value());

    // Poll until the loopback datagram lands; the point is what arrives, not when.
    dgram::result<std::size_t> got{std::unexpected{dgram::would_block}};
    for (int attempt{}; attempt < 1000 && !got.has_value(); ++attempt) {
        got = rx->receive(receiver);
    }
    ASSERT_TRUE(got.has_value()) << "datagram never arrived";
    ASSERT_EQ(*got, 1u);

    const auto view{rx->datagrams()};
    const auto d{*view.begin()};
    EXPECT_TRUE(d.intact()) << "a receive after EAGAIN must not come back truncated";
    EXPECT_EQ(d.payload().size(), text.size());
    EXPECT_TRUE(d.from().is_v4());
    EXPECT_EQ(d.from().port(), sender.local_address()->port());
}

TEST(Transmit, StageRefusesWhenFull) {
    libmem::arena arena{tx_batch::footprint()};
    auto tx{tx_batch::carve(arena)};
    ASSERT_TRUE(tx.has_value());

    const auto to{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", 9)};
    for (std::size_t i{}; i < capacity; ++i) {
        EXPECT_TRUE(tx->stage(bytes_of("x"), to));
    }
    EXPECT_TRUE(tx->full());
    EXPECT_FALSE(tx->stage(bytes_of("x"), to));
}

TEST(Transmit, StageCopyRefusesOversizedPayload) {
    libmem::arena arena{tx_batch::footprint()};
    auto tx{tx_batch::carve(arena)};
    ASSERT_TRUE(tx.has_value());

    const std::string big(slot + 1, 'x');
    const auto to{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", 9)};
    EXPECT_FALSE(tx->stage_copy(bytes_of(big), to));
}

TEST(Error, PipeCombinatorsChainLeftToRight) {
    const dgram::result<int> ok{21};
    const auto doubled{ok | dgram::map([](const int v) { return v * 2; })};
    ASSERT_TRUE(doubled.has_value());
    EXPECT_EQ(*doubled, 42);

    const dgram::result<int> bad{std::unexpected{dgram::errc{EAGAIN}}};
    int side_effects{};
    const auto untouched{bad | dgram::tap([&](int) { ++side_effects; }) | dgram::map([](const int v) { return v * 2; })};
    EXPECT_FALSE(untouched.has_value());
    EXPECT_EQ(side_effects, 0) << "tap must not run on the error path";
    EXPECT_EQ(untouched.error(), dgram::errc{EAGAIN});

    const auto recovered{bad | dgram::recover([](dgram::errc) { return dgram::result<int>{7}; })};
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, 7);
}

/* A failed send is not a short send: nothing left, so the caller must still be
   holding everything it staged. Clearing here would lose datagrams silently for
   any caller that stages by reference and then drops its own copy. */
TEST(TransmitBatch, AFailedFlushLeavesTheBatchStaged) {
    libmem::arena arena{dgram::transmit_batch<4, 64>::footprint()};
    auto tx{dgram::transmit_batch<4, 64>::carve(arena)};
    ASSERT_TRUE(tx.has_value());

    // A socket that cannot send: never bound, and pointed at nothing reachable.
    auto sender{dgram::socket::open<dgram::nonblocking>(dgram::family::inet4)};
    ASSERT_TRUE(sender.has_value());
    const auto unreachable{*dgram::endpoint::parse(dgram::family::inet6, "::1", 9)};

    const std::array<std::byte, 4> payload{};
    ASSERT_TRUE(tx->stage_copy(payload, unreachable));
    ASSERT_EQ(tx->staged(), 1u);

    const auto sent{tx->flush(*sender)};
    ASSERT_FALSE(sent.has_value()) << "a v6 destination on a v4 socket cannot be sent";
    EXPECT_EQ(tx->staged(), 1u) << "the caller has to be able to see what did not go";

    tx->discard();
    EXPECT_EQ(tx->staged(), 0u);
}

} // namespace
