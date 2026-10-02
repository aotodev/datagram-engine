// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file metadata_tests.cpp
 * @brief Ancillary data over loopback: destination address and ECN, both
 *        families, plus the absent-cmsg and truncated-control-buffer paths.
 */
#include <gtest/gtest.h>

#include <cerrno>
#include <netinet/in.h>

import std;
import libmem;
import dgram;

namespace {

constexpr std::size_t capacity{8};
constexpr std::size_t slot{512};

using meta_set = dgram::features<dgram::pktinfo, dgram::ecn>;
using tx_set = dgram::features<dgram::pktinfo, dgram::traffic_class>;
using rx_batch = dgram::receive_batch<capacity, slot, meta_set>;
using tx_batch = dgram::transmit_batch<capacity, slot, tx_set>;

std::span<const std::byte> bytes_of(std::string_view s) noexcept {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

/** A receiver reporting metadata and a sender aimed at it, over loopback. */
template <typename RxBatch = rx_batch, typename TxBatch = tx_batch> struct fixture {
    dgram::family fam;
    libmem::arena arena{RxBatch::footprint() + TxBatch::footprint()};
    dgram::socket receiver;
    dgram::socket sender;
    dgram::endpoint target{};

    explicit fixture(dgram::family f)
        : fam{f}, receiver{std::move(*dgram::socket::open<dgram::reuse_addr, dgram::receive_metadata<dgram::pktinfo, dgram::ecn>>(f))},
          sender{std::move(*dgram::socket::open<>(f))} {
        EXPECT_TRUE(receiver.bind(dgram::endpoint::any(f, 0)).has_value());
        const auto local{receiver.local_address()};
        EXPECT_TRUE(local.has_value());
        const char* loopback{f == dgram::family::inet4 ? "127.0.0.1" : "::1"};
        target = *dgram::endpoint::parse(f, loopback, local->port());
    }
};

/* ============================================================================
 * Receive
 * ============================================================================ */

TEST(Metadata, ReportsDestinationAddressOverIPv4) {
    fixture net{dgram::family::inet4};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    ASSERT_TRUE(tx->stage_copy(bytes_of("where did i land"), net.target));
    ASSERT_TRUE(tx->flush(net.sender).drained());

    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    const auto view{rx->datagrams()};
    const auto d{*view.begin()};
    ASSERT_TRUE(d.intact());

    const auto meta{d.meta()};
    const auto& local{meta.get<dgram::pktinfo>()};
    ASSERT_TRUE(local.has_value()) << "IP_PKTINFO was enabled, so it must arrive";
    EXPECT_TRUE(local->address.is_v4());
    EXPECT_EQ(local->address.text().substr(0, 9), "127.0.0.1");
}

TEST(Metadata, ReportsDestinationAddressOverIPv6) {
    fixture net{dgram::family::inet6};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    ASSERT_TRUE(tx->stage_copy(bytes_of("v6 please"), net.target));
    ASSERT_TRUE(tx->flush(net.sender).drained());

    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    const auto view{rx->datagrams()};
    const auto meta{(*view.begin()).meta()};
    const auto& local{meta.get<dgram::pktinfo>()};
    ASSERT_TRUE(local.has_value());
    EXPECT_TRUE(local->address.is_v6()) << "a v6 socket reports IPV6_PKTINFO";
}

/* The widths differ between families, which is the whole reason parse branches
   on the length the kernel wrote rather than on the family. */
class EcnRoundTrip : public testing::TestWithParam<dgram::family> {};

TEST_P(EcnRoundTrip, MarkedCodepointSurvives) {
    fixture net{GetParam()};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    for (const auto marked : {dgram::ecn_codepoint::ect0, dgram::ecn_codepoint::ect1, dgram::ecn_codepoint::ce}) {
        dgram::control<tx_set> ancillary{};
        ancillary.set<dgram::traffic_class>({.ecn = marked});
        ASSERT_TRUE(tx->stage_copy(bytes_of("marked"), net.target, ancillary));
        ASSERT_TRUE(tx->flush(net.sender).drained());

        ASSERT_EQ(*rx->receive(net.receiver), 1u);
        const auto view{rx->datagrams()};
        const auto meta{(*view.begin()).meta()};
        const auto& got{meta.get<dgram::ecn>()};
        ASSERT_TRUE(got.has_value()) << "traffic class reporting was enabled";
        EXPECT_EQ(*got, marked) << "codepoint " << static_cast<int>(std::to_underlying(marked));
    }
}

INSTANTIATE_TEST_SUITE_P(BothFamilies, EcnRoundTrip, testing::Values(dgram::family::inet4, dgram::family::inet6),
    [](const auto& p) { return p.param == dgram::family::inet4 ? "IPv4" : "IPv6"; });

TEST(Metadata, UnmarkedDatagramReadsAsNotEct) {
    fixture net{dgram::family::inet4};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    ASSERT_TRUE(tx->stage_copy(bytes_of("plain"), net.target));
    ASSERT_TRUE(tx->flush(net.sender).drained());

    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    const auto view{rx->datagrams()};
    const auto& got{(*view.begin()).meta().get<dgram::ecn>()};
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, dgram::ecn_codepoint::not_ect);
}

/* ============================================================================
 * Traffic class: DSCP travels with ECN
 * ============================================================================ */

using class_set = dgram::features<dgram::ecn, dgram::traffic_class>;
using class_rx = dgram::receive_batch<capacity, slot, class_set>;

/** A receiver reporting the traffic class, and a plain sender aimed at it. */
struct class_fixture {
    libmem::arena arena{class_rx::footprint() + tx_batch::footprint()};
    dgram::socket receiver;
    dgram::socket sender;
    dgram::endpoint target{};

    explicit class_fixture(const dgram::family f)
        : receiver{std::move(*dgram::socket::open<dgram::receive_metadata<dgram::traffic_class>>(f))}, sender{std::move(*dgram::socket::open<>(f))} {
        EXPECT_TRUE(receiver.bind(dgram::endpoint::any(f, 0)).has_value());
        target = *dgram::endpoint::parse(f, f == dgram::family::inet4 ? "127.0.0.1" : "::1", receiver.local_address()->port());
    }

    /** The DSCP a socket applies to datagrams that carry no traffic class of their own. */
    void set_socket_dscp(const dgram::family f, const dgram::dscp d) const {
        const int byte{dgram::marking{d, dgram::ecn_codepoint::not_ect}.byte()};
        const int level{f == dgram::family::inet4 ? IPPROTO_IP : IPPROTO_IPV6};
        const int name{f == dgram::family::inet4 ? IP_TOS : IPV6_TCLASS};
        ASSERT_EQ(::setsockopt(sender.native(), level, name, &byte, sizeof(byte)), 0);
    }
};

class TrafficClass : public testing::TestWithParam<dgram::family> {};

/* The bug this guards: marking ECN used to write the ECN bits as the whole byte,
   so every ECN-marked datagram went out as best effort. */
TEST_P(TrafficClass, DscpAndEcnArriveTogether) {
    class_fixture net{GetParam()};
    auto rx{class_rx::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    for (const auto sent : {dgram::marking{dgram::dscp::ef, dgram::ecn_codepoint::ect0}, dgram::marking{dgram::dscp::af41, dgram::ecn_codepoint::ce},
             dgram::marking{dgram::dscp::cs1, dgram::ecn_codepoint::not_ect}}) {
        dgram::control<tx_set> ancillary{};
        ancillary.set<dgram::traffic_class>(sent);
        ASSERT_TRUE(tx->stage_copy(bytes_of("marked"), net.target, ancillary));
        ASSERT_TRUE(tx->flush(net.sender).drained());

        ASSERT_EQ(*rx->receive(net.receiver), 1u);
        const auto meta{(*rx->datagrams().begin()).meta()};
        ASSERT_TRUE(meta.get<dgram::traffic_class>().has_value());
        EXPECT_EQ(meta.get<dgram::traffic_class>()->byte(), sent.byte());
        ASSERT_TRUE(meta.get<dgram::ecn>().has_value()) << "ecn reads the same message";
        EXPECT_EQ(*meta.get<dgram::ecn>(), sent.ecn);
    }
}

/* The per-datagram byte owns the whole traffic class: a socket-level DSCP applies
   only to datagrams that carry none. */
TEST_P(TrafficClass, PerDatagramMarkingReplacesTheSocketDscp) {
    class_fixture net{GetParam()};
    net.set_socket_dscp(GetParam(), dgram::dscp::ef);
    auto rx{class_rx::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    ASSERT_TRUE(tx->stage_copy(bytes_of("socket default"), net.target));
    ASSERT_TRUE(tx->flush(net.sender).drained());
    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    EXPECT_EQ((*rx->datagrams().begin()).meta().get<dgram::traffic_class>()->dscp, dgram::dscp::ef);

    dgram::control<tx_set> ancillary{};
    ancillary.set<dgram::traffic_class>({.ecn = dgram::ecn_codepoint::ect0});
    ASSERT_TRUE(tx->stage_copy(bytes_of("own marking"), net.target, ancillary));
    ASSERT_TRUE(tx->flush(net.sender).drained());
    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    const auto got{*(*rx->datagrams().begin()).meta().get<dgram::traffic_class>()};
    EXPECT_EQ(got.dscp, dgram::dscp::df) << "the datagram said df, and that wins over the socket";
    EXPECT_EQ(got.ecn, dgram::ecn_codepoint::ect0);
}

INSTANTIATE_TEST_SUITE_P(BothFamilies, TrafficClass, testing::Values(dgram::family::inet4, dgram::family::inet6),
    [](const auto& p) { return p.param == dgram::family::inet4 ? "IPv4" : "IPv6"; });

/* ============================================================================
 * Dual-stack sockets: IPv4 peers on a v6 socket
 * ============================================================================ */

/* Explicit, because the default is the net.ipv6.bindv6only sysctl. */
struct dual_stack {
    [[nodiscard]] static dgram::result<> apply(const int fd, dgram::family) noexcept {
        const int off{0};
        if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0) {
            return std::unexpected{dgram::errc{errno}};
        }
        return {};
    }
};

constexpr std::array marked_codepoints{dgram::ecn_codepoint::ect0, dgram::ecn_codepoint::ect1, dgram::ecn_codepoint::ce};

/* IPv4 arrivals take the kernel's IPv4 path, which reports IP_TOS and never
   IPV6_TCLASS, so the v6 socket has to ask for both. */
TEST(DualStack, ReportsEcnForIPv4Arrivals) {
    fixture net{dgram::family::inet4};
    dgram::socket receiver{std::move(*dgram::socket::open<dual_stack, dgram::receive_metadata<dgram::pktinfo, dgram::ecn>>(dgram::family::inet6))};
    ASSERT_TRUE(receiver.bind(dgram::endpoint::any(dgram::family::inet6, 0)).has_value());
    const auto target{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", receiver.local_address()->port())};

    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    for (const auto marked : marked_codepoints) {
        dgram::control<tx_set> ancillary{};
        ancillary.set<dgram::traffic_class>({.ecn = marked});
        ASSERT_TRUE(tx->stage_copy(bytes_of("from v4"), target, ancillary));
        ASSERT_TRUE(tx->flush(net.sender).drained());

        ASSERT_EQ(*rx->receive(receiver), 1u);
        const auto d{*rx->datagrams().begin()};
        ASSERT_TRUE(d.intact()) << "IP_TOS and IPV6_PKTINFO both fit the control buffer";
        EXPECT_TRUE(d.from().is_v4_mapped());
        const auto meta{d.meta()};
        ASSERT_TRUE(meta.get<dgram::ecn>().has_value()) << "codepoint " << static_cast<int>(std::to_underlying(marked));
        EXPECT_EQ(*meta.get<dgram::ecn>(), marked);
        ASSERT_TRUE(meta.get<dgram::pktinfo>().has_value());
        EXPECT_TRUE(meta.get<dgram::pktinfo>()->address.is_v4_mapped());
    }
}

/* Sending to a v4-mapped peer goes down the IPv4 path, which ignores IPV6_TCLASS. */
TEST(DualStack, MarksEcnOnDatagramsToV4MappedPeers) {
    fixture net{dgram::family::inet4};
    dgram::socket sender{std::move(*dgram::socket::open<dual_stack>(dgram::family::inet6))};
    const auto target{*dgram::endpoint::parse(dgram::family::inet6, "::ffff:127.0.0.1", net.target.port())};

    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    for (const auto marked : marked_codepoints) {
        dgram::control<tx_set> ancillary{};
        ancillary.set<dgram::traffic_class>({.ecn = marked});
        ASSERT_TRUE(tx->stage_copy(bytes_of("to v4-mapped"), target, ancillary));
        const auto sent{tx->flush(sender)};
        ASSERT_EQ(sent.sent, 1u) << sent.last_rejection.transform(dgram::describe).value_or("stalled");

        ASSERT_EQ(*rx->receive(net.receiver), 1u);
        const auto& got{(*rx->datagrams().begin()).meta().get<dgram::ecn>()};
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(*got, marked) << "codepoint " << static_cast<int>(std::to_underlying(marked));
    }
}

/* The server pattern: answer an IPv4 client from the local address it reached,
   reflecting its marking, by handing the arrival's local_info straight back. */
TEST(DualStack, RepliesToAnIPv4PeerFromTheArrivalAddress) {
    fixture client{dgram::family::inet4};
    dgram::socket server{std::move(*dgram::socket::open<dual_stack, dgram::receive_metadata<dgram::pktinfo, dgram::ecn>>(dgram::family::inet6))};
    ASSERT_TRUE(server.bind(dgram::endpoint::any(dgram::family::inet6, 0)).has_value());
    const auto server_port{server.local_address()->port()};
    const auto server_target{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", server_port)};

    libmem::arena server_arena{rx_batch::footprint() + tx_batch::footprint()};
    auto client_rx{rx_batch::carve(client.arena)};
    auto client_tx{tx_batch::carve(client.arena)};
    auto server_rx{rx_batch::carve(server_arena)};
    auto server_tx{tx_batch::carve(server_arena)};
    ASSERT_TRUE(client_rx.has_value() && client_tx.has_value() && server_rx.has_value() && server_tx.has_value());

    // The client's receive socket sends too, so the reply comes back to it.
    dgram::control<tx_set> request{};
    request.set<dgram::traffic_class>({.ecn = dgram::ecn_codepoint::ect1});
    ASSERT_TRUE(client_tx->stage_copy(bytes_of("ping"), server_target, request));
    ASSERT_TRUE(client_tx->flush(client.receiver).drained());

    ASSERT_EQ(*server_rx->receive(server), 1u);
    const auto arrival{*server_rx->datagrams().begin()};
    const auto meta{arrival.meta()};
    ASSERT_TRUE(meta.get<dgram::pktinfo>().has_value() && meta.get<dgram::ecn>().has_value());

    dgram::control<tx_set> reply{};
    reply.set<dgram::pktinfo>(*meta.get<dgram::pktinfo>());
    reply.set<dgram::traffic_class>({.ecn = *meta.get<dgram::ecn>()});
    ASSERT_TRUE(server_tx->stage_copy(bytes_of("pong"), arrival.from(), reply));
    const auto sent{server_tx->flush(server)};
    ASSERT_EQ(sent.sent, 1u) << sent.last_rejection.transform(dgram::describe).value_or("stalled");

    ASSERT_EQ(*client_rx->receive(client.receiver), 1u);
    const auto back{*client_rx->datagrams().begin()};
    EXPECT_EQ(back.from(), server_target) << "answered from the address the client addressed";
    const auto& ecn{back.meta().get<dgram::ecn>()};
    ASSERT_TRUE(ecn.has_value());
    EXPECT_EQ(*ecn, dgram::ecn_codepoint::ect1) << "the reflected marking survived the v4-mapped send";
}

TEST(DualStack, V6OnlySocketStillEnablesEcn) {
    const auto sock{dgram::socket::open<dgram::v6_only, dgram::receive_metadata<dgram::pktinfo, dgram::ecn>>(dgram::family::inet6)};
    ASSERT_TRUE(sock.has_value()) << dgram::describe(sock.error());
}

/* ============================================================================
 * The paths that are easy to get wrong
 * ============================================================================ */

/* A feature in the set whose option was never enabled must read as absent, not
   as a zeroed value: the kernel simply sends fewer control messages. */
TEST(Metadata, FeatureNeverEnabledReadsAsAbsent) {
    libmem::arena arena{rx_batch::footprint() + tx_batch::footprint()};
    // Only ECN reporting is switched on; pktinfo stays in the set but unenabled.
    dgram::socket receiver{std::move(*dgram::socket::open<dgram::reuse_addr, dgram::receive_metadata<dgram::ecn>>(dgram::family::inet4))};
    dgram::socket sender{std::move(*dgram::socket::open<>(dgram::family::inet4))};
    ASSERT_TRUE(receiver.bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());
    const auto target{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", receiver.local_address()->port())};

    auto rx{rx_batch::carve(arena)};
    auto tx{tx_batch::carve(arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    ASSERT_TRUE(tx->stage_copy(bytes_of("only ecn"), target));
    ASSERT_TRUE(tx->flush(sender).drained());

    ASSERT_EQ(*rx->receive(receiver), 1u);
    const auto view{rx->datagrams()};
    const auto meta{(*view.begin()).meta()};
    EXPECT_FALSE(meta.get<dgram::pktinfo>().has_value()) << "absent, not a zeroed local_info";
    EXPECT_TRUE(meta.get<dgram::ecn>().has_value());
}

/* An undersized control buffer must set MSG_CTRUNC and the parser must stay
   inside msg_controllen rather than walking off the end of what arrived. */
TEST(Metadata, UndersizedControlBufferTruncatesWithoutOverreading) {
    // A set sized for ECN alone, receiving a datagram that also carries pktinfo.
    using narrow_set = dgram::features<dgram::ecn>;
    using narrow_rx = dgram::receive_batch<capacity, slot, narrow_set>;

    libmem::arena arena{narrow_rx::footprint() + tx_batch::footprint()};
    dgram::socket receiver{std::move(*dgram::socket::open<dgram::reuse_addr, dgram::receive_metadata<dgram::pktinfo, dgram::ecn>>(dgram::family::inet4))};
    dgram::socket sender{std::move(*dgram::socket::open<>(dgram::family::inet4))};
    ASSERT_TRUE(receiver.bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());
    const auto target{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", receiver.local_address()->port())};

    auto rx{narrow_rx::carve(arena)};
    auto tx{tx_batch::carve(arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    ASSERT_TRUE(tx->stage_copy(bytes_of("too much metadata"), target));
    ASSERT_TRUE(tx->flush(sender).drained());

    ASSERT_EQ(*rx->receive(receiver), 1u);
    const auto view{rx->datagrams()};
    const auto d{*view.begin()};

    EXPECT_TRUE(d.control_truncated()) << "the control buffer was too small for what the kernel had";
    EXPECT_FALSE(d.intact());
    EXPECT_FALSE(d.truncated()) << "the payload itself fit";
    EXPECT_EQ(d.payload().size(), std::string_view{"too much metadata"}.size());

    // Whatever survived must parse without reading past msg_controllen. Under
    // ASan this is the assertion that matters; the value itself may be absent.
    const auto meta{d.meta()};
    (void)meta.get<dgram::ecn>();
}

/* Staging without ancillary data must leave msg_controllen at zero, not at
   whatever the previous datagram in the batch happened to build. */
TEST(Metadata, StaleControlLengthDoesNotLeakBetweenDatagrams) {
    fixture net{dgram::family::inet4};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    dgram::control<tx_set> marked{};
    marked.set<dgram::traffic_class>({.ecn = dgram::ecn_codepoint::ce});

    ASSERT_TRUE(tx->stage_copy(bytes_of("marked"), net.target, marked));
    ASSERT_TRUE(tx->flush(net.sender).drained());
    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    {
        const auto view{rx->datagrams()};
        EXPECT_EQ(*(*view.begin()).meta().get<dgram::ecn>(), dgram::ecn_codepoint::ce);
    }

    // Same slot, now with nothing attached.
    ASSERT_TRUE(tx->stage_copy(bytes_of("plain"), net.target));
    ASSERT_TRUE(tx->flush(net.sender).drained());
    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    {
        const auto view{rx->datagrams()};
        const auto d{*view.begin()};
        ASSERT_TRUE(d.intact());
        EXPECT_EQ(*d.meta().get<dgram::ecn>(), dgram::ecn_codepoint::not_ect) << "the previous CE marking must not carry over";
    }
}

/* A batch with no features carves no control buffer and parses nothing. */
TEST(Metadata, EmptyFeatureSetCostsNothing) {
    static_assert(dgram::no_features::control_space == 0);
    static_assert(dgram::receive_batch<8, 64>::geometry.control_bytes == 0);
    static_assert(dgram::receive_batch<8, 64, meta_set>::footprint() > dgram::receive_batch<8, 64>::footprint());
    SUCCEED();
}

} // namespace
