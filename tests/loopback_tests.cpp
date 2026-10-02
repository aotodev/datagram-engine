// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cerrno>
#include <sys/socket.h>

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

TEST(Endpoint, V4MappedTravelsAsIPv4) {
    const auto mapped{*dgram::endpoint::parse(dgram::family::inet6, "::ffff:192.0.2.7", 443)};
    EXPECT_TRUE(mapped.is_v4_mapped());
    EXPECT_EQ(mapped.wire_family(), dgram::family::inet4);

    const auto v6{*dgram::endpoint::parse(dgram::family::inet6, "2001:db8::1", 443)};
    EXPECT_FALSE(v6.is_v4_mapped());
    EXPECT_EQ(v6.wire_family(), dgram::family::inet6);

    const auto compatible{*dgram::endpoint::parse(dgram::family::inet6, "::192.0.2.7", 443)};
    EXPECT_FALSE(compatible.is_v4_mapped()) << "the deprecated v4-compatible form is a plain v6 address";

    const auto v4{*dgram::endpoint::parse(dgram::family::inet4, "192.0.2.7", 443)};
    EXPECT_FALSE(v4.is_v4_mapped());
    EXPECT_EQ(v4.wire_family(), dgram::family::inet4);
    EXPECT_FALSE(dgram::endpoint{}.is_v4_mapped());
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
    ASSERT_TRUE(sent.drained());
    EXPECT_EQ(sent.sent, sent_count);

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
        ASSERT_TRUE(tx->flush(net.sender).drained());

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
    ASSERT_TRUE(tx->flush(net.sender).drained());

    const auto got{rx->receive(net.receiver)};
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(*got, 1u);

    const auto view{rx->datagrams()};
    const auto d{*view.begin()};
    EXPECT_TRUE(d.truncated()) << "an oversized datagram must not look intact";
    EXPECT_FALSE(d.intact());
    EXPECT_EQ(d.payload().size(), 8u);
}

/* With MSG_TRUNC in the receive flags the kernel reports the wire length in
   msg_len, which exceeds the slot. The payload view must still stop at the slot. */
TEST(Loopback, CallerMsgTruncDoesNotStretchThePayloadPastTheSlot) {
    using tiny_rx = dgram::receive_batch<4, 8>;
    loopback net{};
    auto rx{tiny_rx::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value());
    ASSERT_TRUE(tx.has_value());

    ASSERT_TRUE(tx->stage_copy(bytes_of("far longer than eight bytes"), net.target));
    ASSERT_TRUE(tx->flush(net.sender).drained());

    ASSERT_EQ(*rx->receive(net.receiver, MSG_TRUNC), 1u);
    const auto d{*rx->datagrams().begin()};
    EXPECT_TRUE(d.truncated());
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
    ASSERT_TRUE(tx->flush(sender).drained());

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

/* ============================================================================
 * Flushing: who an error belongs to
 * ============================================================================ */

/** Stands in for sendmmsg: replays scripted results and records each call's vector length. */
struct scripted_send {
    std::vector<dgram::result<int>> script;
    std::vector<std::size_t> lengths{};

    dgram::result<int> operator()(const std::span<::mmsghdr> pending) {
        lengths.push_back(pending.size());
        const auto next{script.front()};
        script.erase(script.begin());
        return next;
    }
};

/* sendmmsg reports a datagram's error only on the call that starts at it. */
TEST(SendAll, ARejectedDatagramIsSkippedAndTheRestStillGo) {
    std::array<::mmsghdr, 6> msgs{};
    scripted_send send{{2, std::unexpected{dgram::message_too_long}, 3}};

    const auto out{dgram::detail::send_all(msgs, send)};
    EXPECT_EQ(out.sent, 5u);
    EXPECT_EQ(out.rejected, 1u);
    EXPECT_EQ(out.last_rejection, dgram::message_too_long);
    EXPECT_TRUE(out.drained());
    EXPECT_EQ(send.lengths, (std::vector<std::size_t>{6, 4, 3})) << "each call starts where the last one stopped";
}

TEST(SendAll, ASocketErrorStopsAfterWhatWasAlreadySent) {
    std::array<::mmsghdr, 6> msgs{};
    scripted_send send{{2, std::unexpected{dgram::errc{EHOSTUNREACH}}, std::unexpected{dgram::would_block}}};

    const auto out{dgram::detail::send_all(msgs, send)};
    EXPECT_EQ(out.sent, 2u);
    EXPECT_EQ(out.rejected, 1u);
    EXPECT_EQ(out.stalled, dgram::would_block);
    EXPECT_FALSE(out.drained());
    EXPECT_EQ(send.lengths, (std::vector<std::size_t>{6, 4, 3})) << "three entries consumed, three left";
}

TEST(SendAll, EverySocketLevelErrorStalls) {
    for (const auto e : {dgram::would_block, dgram::interrupted, dgram::out_of_memory, dgram::errc{ENOBUFS}, dgram::errc{EBADF}, dgram::errc{ENOTSOCK},
             dgram::errc{EOPNOTSUPP}}) {
        std::array<::mmsghdr, 2> msgs{};
        scripted_send send{{std::unexpected{e}}};
        const auto out{dgram::detail::send_all(msgs, send)};
        EXPECT_EQ(out.stalled, e) << dgram::describe(e);
        EXPECT_EQ(out.sent + out.rejected, 0u);
    }
}

TEST(SendAll, AZeroCountStallsRatherThanSpins) {
    std::array<::mmsghdr, 2> msgs{};
    scripted_send send{{0}};
    const auto out{dgram::detail::send_all(msgs, send)};
    EXPECT_EQ(out.stalled, dgram::would_block);
    EXPECT_EQ(send.lengths.size(), 1u);
}

/* The bug this guards: an error on the first entry used to leave the whole batch
   staged, so one destination the kernel always refuses stopped every reply. */
TEST(TransmitBatch, ARefusedDatagramIsDroppedAndTheRestStillGo) {
    loopback net{};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    const std::vector<std::byte> oversized(70'000); // past the 65507-byte UDP limit: EMSGSIZE
    ASSERT_TRUE(tx->stage(oversized, net.target));
    ASSERT_TRUE(tx->stage_copy(bytes_of("first"), net.target));
    ASSERT_TRUE(tx->stage(oversized, net.target));
    ASSERT_TRUE(tx->stage_copy(bytes_of("second"), net.target));

    const auto sent{tx->flush(net.sender)};
    EXPECT_EQ(sent.sent, 2u);
    EXPECT_EQ(sent.rejected, 2u);
    EXPECT_EQ(sent.last_rejection, dgram::message_too_long);
    EXPECT_TRUE(sent.drained());
    EXPECT_EQ(tx->staged(), 0u);

    ASSERT_EQ(*rx->receive(net.receiver), 2u);
    const auto texts{rx->datagrams() | std::views::transform([](const auto& d) {
        return std::string{reinterpret_cast<const char*>(d.payload().data()), d.payload().size()};
    }) | std::ranges::to<std::vector>()};
    EXPECT_EQ(texts, (std::vector<std::string>{"first", "second"}));
}

TEST(TransmitBatch, ADestinationTheSocketCannotReachIsRejectedNotKept) {
    libmem::arena arena{dgram::transmit_batch<4, 64>::footprint()};
    auto tx{dgram::transmit_batch<4, 64>::carve(arena)};
    ASSERT_TRUE(tx.has_value());
    auto sender{dgram::socket::open<dgram::nonblocking>(dgram::family::inet4)};
    ASSERT_TRUE(sender.has_value());

    const std::array<std::byte, 4> payload{};
    ASSERT_TRUE(tx->stage_copy(payload, *dgram::endpoint::parse(dgram::family::inet6, "::1", 9)));

    const auto sent{tx->flush(*sender)};
    EXPECT_EQ(sent.rejected, 1u) << "a v6 destination on a v4 socket is that datagram's problem";
    EXPECT_TRUE(sent.last_rejection.has_value());
    EXPECT_EQ(tx->staged(), 0u);
}

/* A socket-level error keeps everything staged, in order, and the batch keeps
   accepting entries behind it. MSG_OOB is refused for UDP before any datagram. */
TEST(TransmitBatch, AStalledFlushKeepsTheBatchStagedForTheNextOne) {
    loopback net{};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    ASSERT_TRUE(tx->stage_copy(bytes_of("one"), net.target));
    ASSERT_TRUE(tx->stage_copy(bytes_of("two"), net.target));

    const auto stalled{tx->flush(net.sender, MSG_OOB)};
    EXPECT_EQ(stalled.stalled, dgram::errc{EOPNOTSUPP});
    EXPECT_EQ(stalled.sent + stalled.rejected, 0u);
    EXPECT_EQ(tx->staged(), 2u) << "the caller has to be able to see what did not go";

    ASSERT_TRUE(tx->stage_copy(bytes_of("three"), net.target));
    const auto sent{tx->flush(net.sender)};
    EXPECT_TRUE(sent.drained());
    EXPECT_EQ(sent.sent, 3u);
    EXPECT_EQ(tx->staged(), 0u);

    ASSERT_EQ(*rx->receive(net.receiver), 3u);
    const auto texts{rx->datagrams() | std::views::transform([](const auto& d) {
        return std::string{reinterpret_cast<const char*>(d.payload().data()), d.payload().size()};
    }) | std::ranges::to<std::vector>()};
    EXPECT_EQ(texts, (std::vector<std::string>{"one", "two", "three"}));
}

TEST(TransmitBatch, DiscardDropsAStalledBatch) {
    loopback net{};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(tx.has_value());
    ASSERT_TRUE(tx->stage_copy(bytes_of("dropped"), net.target));
    ASSERT_FALSE(tx->flush(net.sender, MSG_OOB).drained());

    tx->discard();
    EXPECT_EQ(tx->staged(), 0u);
    EXPECT_FALSE(tx->full());
}

} // namespace
