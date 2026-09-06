// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file metadata_tests.cpp
 * @brief Ancillary data over loopback: destination address and ECN, both
 *        families, plus the absent-cmsg and truncated-control-buffer paths.
 */
#include <gtest/gtest.h>

#include <netinet/in.h>

import std;
import libmem;
import dgram;

namespace {

constexpr std::size_t capacity{8};
constexpr std::size_t slot{512};

using meta_set = dgram::features<dgram::pktinfo, dgram::ecn>;
using rx_batch = dgram::receive_batch<capacity, slot, meta_set>;
using tx_batch = dgram::transmit_batch<capacity, slot, meta_set>;

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
    ASSERT_TRUE(tx->flush(net.sender).has_value());

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
    ASSERT_TRUE(tx->flush(net.sender).has_value());

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
        dgram::control<meta_set> ancillary{};
        ancillary.set<dgram::ecn>(marked);
        ASSERT_TRUE(tx->stage_copy(bytes_of("marked"), net.target, ancillary));
        ASSERT_TRUE(tx->flush(net.sender).has_value());

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
    ASSERT_TRUE(tx->flush(net.sender).has_value());

    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    const auto view{rx->datagrams()};
    const auto& got{(*view.begin()).meta().get<dgram::ecn>()};
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, dgram::ecn_codepoint::not_ect);
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
    ASSERT_TRUE(tx->flush(sender).has_value());

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
    ASSERT_TRUE(tx->flush(sender).has_value());

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

    dgram::control<meta_set> marked{};
    marked.set<dgram::ecn>(dgram::ecn_codepoint::ce);

    ASSERT_TRUE(tx->stage_copy(bytes_of("marked"), net.target, marked));
    ASSERT_TRUE(tx->flush(net.sender).has_value());
    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    {
        const auto view{rx->datagrams()};
        EXPECT_EQ(*(*view.begin()).meta().get<dgram::ecn>(), dgram::ecn_codepoint::ce);
    }

    // Same slot, now with nothing attached.
    ASSERT_TRUE(tx->stage_copy(bytes_of("plain"), net.target));
    ASSERT_TRUE(tx->flush(net.sender).has_value());
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
