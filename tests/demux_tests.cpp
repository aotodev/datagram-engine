// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file demux_tests.cpp
 * @brief Keys, the flow table, and routing arrivals to protocol state.
 */
#include <gtest/gtest.h>

#include <netinet/in.h>

import std;
import libmem;
import dgram;

namespace {

using namespace std::chrono_literals;

constexpr std::size_t capacity{8};
constexpr std::size_t gro_slot{1 << 16};
constexpr std::uint16_t mtu{1400};

using rx_set = dgram::features<dgram::pktinfo, dgram::gro>;
using tx_set = dgram::features<dgram::segment>;
using rx_batch = dgram::receive_batch<capacity, gro_slot, rx_set>;
using tx_batch = dgram::transmit_batch<capacity, gro_slot, tx_set>;

dgram::endpoint ep(const char* text, std::uint16_t port, dgram::family fam = dgram::family::inet4) {
    return *dgram::endpoint::parse(fam, text, port);
}

constexpr dgram::hash_seed test_seed{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

template <typename K> std::uint64_t hash_of(const K& k, const dgram::hash_seed seed = test_seed) {
    dgram::siphash h{seed};
    hash_append(h, k);
    return h.finish();
}

/** Records what it was given, so routing can be asserted on. */
struct recorder {
    std::vector<std::string> seen{};

    template <typename Features> void on_datagram(const dgram::arrival<Features>& a) {
        seen.emplace_back(reinterpret_cast<const char*>(a.payload.data()), a.payload.size());
    }
};

/* ============================================================================
 * Keys: hash and equality must agree exactly
 * ============================================================================ */

TEST(EndpointKey, EqualEndpointsHashEqual) {
    EXPECT_EQ(ep("192.0.2.1", 443), ep("192.0.2.1", 443));
    EXPECT_EQ(hash_of(ep("192.0.2.1", 443)), hash_of(ep("192.0.2.1", 443)));
    EXPECT_EQ(std::hash<dgram::endpoint>{}(ep("192.0.2.1", 443)), std::hash<dgram::endpoint>{}(ep("192.0.2.1", 443)));
}

TEST(EndpointKey, DistinctEndpointsAreNotEqual) {
    EXPECT_NE(ep("192.0.2.1", 443), ep("192.0.2.2", 443)) << "address";
    EXPECT_NE(ep("192.0.2.1", 443), ep("192.0.2.1", 444)) << "port";
    EXPECT_NE(ep("192.0.2.1", 443), ep("2001:db8::1", 443, dgram::family::inet6)) << "family";
}

/* The bug this guards: comparing whole sockaddrs also compares sin6_flowinfo,
   which the kernel may populate. The same peer would then look like a new peer
   on every datagram, and a demux table would fill with duplicates. */
TEST(EndpointKey, FlowLabelIsNotIdentity) {
    auto a{ep("2001:db8::1", 443, dgram::family::inet6)};
    auto b{ep("2001:db8::1", 443, dgram::family::inet6)};
    reinterpret_cast<::sockaddr_in6&>(b.mutable_storage()).sin6_flowinfo = 0x12345678;

    EXPECT_EQ(a, b) << "a flow label does not identify a peer";
    EXPECT_EQ(hash_of(a), hash_of(b)) << "the hash must agree with equality";
}

/* Scope, unlike flow label, is identity: fe80::1%eth0 is not fe80::1%eth1. */
TEST(EndpointKey, ScopeIdIsIdentity) {
    auto a{ep("fe80::1", 443, dgram::family::inet6)};
    auto b{ep("fe80::1", 443, dgram::family::inet6)};
    reinterpret_cast<::sockaddr_in6&>(b.mutable_storage()).sin6_scope_id = 3;

    EXPECT_NE(a, b);
    EXPECT_NE(hash_of(a), hash_of(b));
}

/* v4 sockaddrs carry eight bytes of sin_zero padding that must not be compared. */
TEST(EndpointKey, V4PaddingIsNotIdentity) {
    auto a{ep("192.0.2.1", 443)};
    auto b{ep("192.0.2.1", 443)};
    auto& raw{reinterpret_cast<::sockaddr_in&>(b.mutable_storage())};
    std::memset(raw.sin_zero, 0xAB, sizeof(raw.sin_zero));

    EXPECT_EQ(a, b);
    EXPECT_EQ(hash_of(a), hash_of(b));
}

TEST(ByteKey, ComparesAndHashesByContent) {
    const std::array<std::byte, 4> raw{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::array<std::byte, 4> same{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::array<std::byte, 4> other{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{5}};

    const dgram::byte_key<8> a{raw};
    const dgram::byte_key<8> b{same};
    const dgram::byte_key<8> c{other};

    EXPECT_EQ(a, b);
    EXPECT_EQ(hash_of(a), hash_of(b));
    EXPECT_NE(a, c);
    EXPECT_EQ(a.bytes().size(), 4u);
}

/* The length goes in first, so a short key followed by more fields cannot feed
   the same bytes as a longer key followed by fewer. */
TEST(ByteKey, FeedsItsLengthSoCompositesStayDistinct) {
    const std::array<std::byte, 2> ab{std::byte{'a'}, std::byte{'b'}};
    const std::array<std::byte, 3> abc{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    const std::array<std::byte, 1> c{std::byte{'c'}};

    dgram::siphash split{test_seed};
    hash_append(split, dgram::byte_key<4>{ab});
    hash_append(split, dgram::byte_key<4>{c});
    dgram::siphash joined{test_seed};
    hash_append(joined, dgram::byte_key<4>{abc});
    hash_append(joined, dgram::byte_key<4>{});
    EXPECT_NE(split.finish(), joined.finish());
}

TEST(KeyedHash, TheSeedChangesTheDigest) {
    const dgram::peer_key k{ep("192.0.2.1", 443)};
    EXPECT_NE(hash_of(k, test_seed), hash_of(k, dgram::hash_seed{1, 2}));
}

TEST(KeyedHash, RandomSeedsDiffer) {
    const auto a{dgram::random_seed()};
    const auto b{dgram::random_seed()};
    ASSERT_TRUE(a.has_value() && b.has_value());
    EXPECT_NE(*a, *b);
}

/* The flooding case: keys chosen to share a bucket under one seed. Under another
   seed they must spread, or a peer that learns one table's layout owns them all. */
TEST(KeyedHash, KeysCraftedToCollideUnderOneSeedSpreadUnderAnother) {
    constexpr std::size_t buckets{64};
    std::vector<dgram::peer_key> crafted{};
    for (std::uint32_t port{1}; crafted.size() < 32 && port <= 0xFFFF; ++port) {
        const dgram::peer_key k{ep("192.0.2.1", static_cast<std::uint16_t>(port))};
        if ((hash_of(k, test_seed) & (buckets - 1)) == 0) {
            crafted.push_back(k);
        }
    }
    ASSERT_EQ(crafted.size(), 32u);

    std::set<std::uint64_t> spread{};
    for (const auto& k : crafted) {
        spread.insert(hash_of(k, dgram::hash_seed{42, 7}) & (buckets - 1));
    }
    EXPECT_GT(spread.size(), 16u) << "32 keys over 64 buckets should land in far more than half as many buckets";
}

/* ============================================================================
 * The flow table
 * ============================================================================ */

using table_type = dgram::flow_table<dgram::peer_key, int, 64>;

TEST(FlowTable, InsertFindErase) {
    libmem::arena arena{table_type::footprint()};
    auto t{table_type::carve(arena)};
    ASSERT_TRUE(t.has_value());

    const dgram::peer_key k{ep("192.0.2.1", 443)};
    EXPECT_EQ(t->find(k), nullptr);
    ASSERT_NE(t->insert(k, 7), nullptr);
    ASSERT_NE(t->find(k), nullptr);
    EXPECT_EQ(*t->find(k), 7);
    EXPECT_EQ(t->size(), 1u);

    EXPECT_TRUE(t->erase(k));
    EXPECT_EQ(t->find(k), nullptr);
    EXPECT_EQ(t->size(), 0u);
    EXPECT_FALSE(t->erase(k)) << "erasing what is not there is not an error, just false";
}

TEST(FlowTable, InsertOverwritesWithoutGrowing) {
    libmem::arena arena{table_type::footprint()};
    auto t{table_type::carve(arena)};
    ASSERT_TRUE(t.has_value());

    const dgram::peer_key k{ep("192.0.2.1", 443)};
    ASSERT_NE(t->insert(k, 1), nullptr);
    ASSERT_NE(t->insert(k, 2), nullptr);
    EXPECT_EQ(*t->find(k), 2);
    EXPECT_EQ(t->size(), 1u);
}

TEST(FlowTable, RefusesInsertAtCapacity) {
    libmem::arena arena{table_type::footprint()};
    auto t{table_type::carve(arena)};
    ASSERT_TRUE(t.has_value());

    for (std::size_t i{}; i < table_type::max_size; ++i) {
        const dgram::peer_key k{ep("192.0.2.1", static_cast<std::uint16_t>(1000 + i))};
        ASSERT_NE(t->insert(k, static_cast<int>(i)), nullptr) << "at " << i;
    }
    EXPECT_TRUE(t->full());
    EXPECT_EQ(t->size(), table_type::max_size);

    const dgram::peer_key overflow{ep("198.51.100.1", 1)};
    EXPECT_EQ(t->insert(overflow, 0), nullptr) << "a full table refuses rather than degrading";
    EXPECT_EQ(t->find(overflow), nullptr);

    // Everything inserted before the refusal must still be findable.
    for (std::size_t i{}; i < table_type::max_size; ++i) {
        const dgram::peer_key k{ep("192.0.2.1", static_cast<std::uint16_t>(1000 + i))};
        ASSERT_NE(t->find(k), nullptr) << "lost entry " << i;
        EXPECT_EQ(*t->find(k), static_cast<int>(i));
    }
}

/* Backward-shift deletion means no tombstones, so heavy churn must not degrade
   the table or lose entries whose probe chain crossed a removed slot. */
TEST(FlowTable, SurvivesHeavyChurn) {
    libmem::arena arena{table_type::footprint()};
    auto t{table_type::carve(arena)};
    ASSERT_TRUE(t.has_value());

    std::mt19937 rng{12345};
    std::map<std::uint16_t, int> reference{};

    for (int round{}; round < 20'000; ++round) {
        const auto port{static_cast<std::uint16_t>(1 + (rng() % 200))};
        const dgram::peer_key k{ep("192.0.2.1", port)};

        if ((rng() % 3) == 0) {
            const bool erased{t->erase(k)};
            EXPECT_EQ(erased, reference.erase(port) == 1) << "round " << round;
        } else if (reference.size() < table_type::max_size || reference.contains(port)) {
            const int value{static_cast<int>(rng())};
            ASSERT_NE(t->insert(k, value), nullptr) << "round " << round;
            reference[port] = value;
        }
        ASSERT_EQ(t->size(), reference.size()) << "round " << round;
    }

    for (const auto& [port, value] : reference) {
        const dgram::peer_key k{ep("192.0.2.1", port)};
        ASSERT_NE(t->find(k), nullptr) << "port " << port;
        EXPECT_EQ(*t->find(k), value);
    }
}

TEST(FlowTable, EntriesViewSeesEveryLiveFlow) {
    libmem::arena arena{table_type::footprint()};
    auto t{table_type::carve(arena)};
    ASSERT_TRUE(t.has_value());

    for (int i{}; i < 10; ++i) {
        ASSERT_NE(t->insert(dgram::peer_key{ep("192.0.2.1", static_cast<std::uint16_t>(100 + i))}, i), nullptr);
    }
    EXPECT_TRUE(t->erase(dgram::peer_key{ep("192.0.2.1", 105)}));

    std::vector<int> values{};
    for (const auto& [key, value] : t->entries()) {
        values.push_back(value);
    }
    std::ranges::sort(values);
    EXPECT_EQ(values, (std::vector<int>{0, 1, 2, 3, 4, 6, 7, 8, 9}));
}

/* The table must bucket by SipHash under its own seed: entries() walks slot
   order, so keys whose seeded buckets are all distinct come back sorted by them. */
TEST(FlowTable, BucketsBySipHashUnderItsSeed) {
    libmem::arena arena{table_type::footprint()};
    auto t{table_type::carve(arena, test_seed)};
    ASSERT_TRUE(t.has_value());

    std::map<std::uint64_t, int> by_bucket{};
    for (int port{1}; by_bucket.size() < 12; ++port) {
        by_bucket.try_emplace(hash_of(dgram::peer_key{ep("192.0.2.1", static_cast<std::uint16_t>(port))}) & 63, port);
    }
    for (const auto& [bucket, port] : by_bucket | std::views::reverse) {
        ASSERT_NE(t->insert(dgram::peer_key{ep("192.0.2.1", static_cast<std::uint16_t>(port))}, port), nullptr);
    }

    std::vector<int> walked{};
    for (const auto& [key, value] : t->entries()) {
        walked.push_back(value);
    }
    EXPECT_EQ(walked, (by_bucket | std::views::values | std::ranges::to<std::vector>()));
}

TEST(FlowTable, CarveFailsCleanlyOnASmallArena) {
    libmem::arena arena{64};
    EXPECT_FALSE(table_type::carve(arena).has_value());
}

/* ============================================================================
 * Projections
 * ============================================================================ */

template <typename Features = dgram::no_features>
dgram::arrival<Features> make_arrival(std::span<const std::byte> payload, const dgram::endpoint& from, const dgram::metadata<Features>& meta) {
    return {payload, from, meta, 0ns};
}

TEST(Projection, ByPeerKeysOnTheSender) {
    const dgram::metadata<dgram::no_features> meta{};
    const auto from{ep("192.0.2.9", 5000)};
    const auto key{dgram::by_peer{}(make_arrival({}, from, meta))};
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key->remote, from);
}

/* The QUIC-shaped case: a connection id at a fixed offset, which survives the
   peer changing address in a way the 4-tuple does not. */
TEST(Projection, ByPayloadIdReadsTheIdentifier) {
    std::array<std::byte, 16> payload{};
    for (auto [i, b] : std::views::enumerate(payload)) {
        b = static_cast<std::byte>(i);
    }
    const dgram::metadata<dgram::no_features> meta{};
    const auto from{ep("192.0.2.9", 5000)};

    const dgram::by_payload_id<1, 8> projection{};
    const auto key{projection(make_arrival(payload, from, meta))};
    ASSERT_TRUE(key.has_value());
    ASSERT_EQ(key->bytes().size(), 8u);
    EXPECT_EQ(key->bytes()[0], std::byte{1}) << "reads from the given offset";
    EXPECT_EQ(key->bytes()[7], std::byte{8});
}

TEST(Projection, ByPayloadIdDeclinesAShortDatagram) {
    const dgram::metadata<dgram::no_features> meta{};
    const auto from{ep("192.0.2.9", 5000)};
    const dgram::by_payload_id<1, 8> projection{};

    std::array<std::byte, 4> too_short{};
    EXPECT_FALSE(projection(make_arrival(too_short, from, meta)).has_value()) << "must decline, not read past the end";
    EXPECT_FALSE(projection(make_arrival({}, from, meta)).has_value());

    std::array<std::byte, 9> exactly_enough{};
    EXPECT_TRUE(projection(make_arrival(exactly_enough, from, meta)).has_value());
}

/* ============================================================================
 * Routing over the wire
 * ============================================================================ */

struct fixture {
    libmem::arena arena{rx_batch::footprint() + tx_batch::footprint() + 4096};
    dgram::socket receiver{std::move(
        *dgram::socket::open<dgram::reuse_addr, dgram::recv_buffer<1 << 21>, dgram::receive_metadata<dgram::pktinfo, dgram::gro>>(dgram::family::inet4))};
    dgram::endpoint target{};

    fixture() {
        EXPECT_TRUE(receiver.bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());
        target = ep("127.0.0.1", receiver.local_address()->port());
    }
};

TEST(Route, DeliversToTheMatchingFlowAndReportsTheRest) {
    fixture net{};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    auto known{dgram::socket::open<>(dgram::family::inet4)};
    auto stranger{dgram::socket::open<>(dgram::family::inet4)};
    ASSERT_TRUE(known.has_value() && stranger.has_value());
    ASSERT_TRUE(known->bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());
    ASSERT_TRUE(stranger->bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());

    using rec_table = dgram::flow_table<dgram::peer_key, recorder*, 64>;
    libmem::arena table_arena{rec_table::footprint()};
    auto table{rec_table::carve(table_arena)};
    ASSERT_TRUE(table.has_value());

    recorder sink{};
    const dgram::peer_key known_key{ep("127.0.0.1", known->local_address()->port())};
    ASSERT_NE(table->insert(known_key, &sink), nullptr);

    const auto say = [&](dgram::socket& s, std::string_view text) {
        ASSERT_TRUE(tx->stage_copy({reinterpret_cast<const std::byte*>(text.data()), text.size()}, net.target));
        ASSERT_TRUE(tx->flush(s).has_value());
    };
    say(*known, "from-known");
    say(*stranger, "from-stranger");

    std::size_t received{};
    for (int attempt{}; attempt < 100 && received < 2; ++attempt) {
        const auto got{rx->receive(net.receiver)};
        if (!got) {
            continue;
        }
        received += *got;

        std::vector<std::string> unmatched{};
        const auto counts{dgram::route(*rx, dgram::by_peer{}, *table, 0ns,
            [&](const auto& a, const auto&) { unmatched.emplace_back(reinterpret_cast<const char*>(a.payload.data()), a.payload.size()); })};
        EXPECT_EQ(counts.delivered + counts.unmatched, *got);
        for (const auto& text : unmatched) {
            EXPECT_EQ(text, "from-stranger");
        }
    }

    ASSERT_EQ(received, 2u);
    EXPECT_EQ(sink.seen, (std::vector<std::string>{"from-known"})) << "only the known peer's datagram reaches the sink";
}

/* The case slot-level dispatch silently gets wrong: one GRO slot holding
   datagrams that belong to different flows. */
TEST(Route, CoalescedSlotRoutesPerDatagramNotPerSlot) {
    fixture net{};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    auto sender{dgram::socket::open<>(dgram::family::inet4)};
    ASSERT_TRUE(sender.has_value());
    ASSERT_TRUE(sender->bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());

    // Key on a byte inside the payload, so datagrams from one peer can belong to
    // different flows: exactly the QUIC connection-id shape.
    using id_table = dgram::flow_table<dgram::byte_key<1>, recorder*, 64>;
    libmem::arena table_arena{id_table::footprint()};
    auto table{id_table::carve(table_arena)};
    ASSERT_TRUE(table.has_value());

    recorder alpha{};
    recorder beta{};
    const auto key_of = [](std::byte id) { return dgram::byte_key<1>{std::span<const std::byte>{&id, 1}}; };
    ASSERT_NE(table->insert(key_of(std::byte{'A'}), &alpha), nullptr);
    ASSERT_NE(table->insert(key_of(std::byte{'B'}), &beta), nullptr);

    // Four segments, alternating connection ids, in one GSO write.
    constexpr std::size_t segments{4};
    std::vector<std::byte> payload(segments * mtu);
    for (std::size_t s{}; s < segments; ++s) {
        payload[s * mtu] = (s % 2 == 0) ? std::byte{'A'} : std::byte{'B'};
    }

    dgram::control<tx_set> ancillary{};
    ancillary.set<dgram::segment>(mtu);
    ASSERT_TRUE(tx->stage(payload, net.target, ancillary));
    ASSERT_TRUE(tx->flush(*sender).has_value());

    ASSERT_EQ(*rx->receive(net.receiver), 1u) << "GRO should coalesce into one slot";
    {
        const auto view{rx->datagrams()};
        ASSERT_TRUE((*view.begin()).meta().get<dgram::gro>().has_value()) << "test is meaningless without coalescing";
    }

    const auto counts{dgram::route(*rx, dgram::by_payload_id<0, 1>{}, *table, 0ns)};
    EXPECT_EQ(counts.delivered, segments) << "each datagram in the slot must be routed on its own key";
    EXPECT_EQ(counts.unmatched, 0u);
    EXPECT_EQ(alpha.seen.size(), 2u);
    EXPECT_EQ(beta.seen.size(), 2u);
}

TEST(Route, TruncatedDatagramsAreCountedNotDelivered) {
    using tiny_rx = dgram::receive_batch<capacity, 8, rx_set>;
    fixture net{};
    auto rx{tiny_rx::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    auto sender{dgram::socket::open<>(dgram::family::inet4)};
    ASSERT_TRUE(sender.has_value());

    using rec_table = dgram::flow_table<dgram::peer_key, recorder*, 64>;
    libmem::arena table_arena{rec_table::footprint()};
    auto table{rec_table::carve(table_arena)};
    ASSERT_TRUE(table.has_value());

    constexpr std::string_view text{"far longer than eight bytes"};
    ASSERT_TRUE(tx->stage_copy({reinterpret_cast<const std::byte*>(text.data()), text.size()}, net.target));
    ASSERT_TRUE(tx->flush(*sender).has_value());

    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    const auto counts{dgram::route(*rx, dgram::by_peer{}, *table, 0ns)};
    EXPECT_EQ(counts.dropped, 1u) << "a truncated datagram must never reach a protocol";
    EXPECT_EQ(counts.delivered, 0u);
    EXPECT_EQ(counts.unmatched, 0u);
}

/* ============================================================================
 * Accepting a flow, and passing something to the sink
 *
 * Both exist because a protocol has to do them and could not: the callback that
 * accepts a connection had no way to say so, and a sink had no way to be given
 * anything at the call.
 * ============================================================================ */

/** A sink that wants somewhere to put what it was given. */
struct forwarding_recorder {
    template <typename Features> void on_datagram(const dgram::arrival<Features>& a, std::vector<std::string>& into) {
        into.emplace_back(reinterpret_cast<const char*>(a.payload.data()), a.payload.size());
    }
};

struct routing_fixture {
    fixture net{};
    libmem::arena table_arena{dgram::flow_table<dgram::peer_key, recorder*, 64>::footprint()};

    /** Send `text` from `s` and route whatever turns up, returning the counts. */
    template <typename Table, typename Unmatched, typename... Context>
    dgram::routed exchange(rx_batch& rx, tx_batch& tx, dgram::socket& s, std::string_view text, Table& table, Unmatched&& unmatched, Context&&... context) {
        EXPECT_TRUE(tx.stage_copy({reinterpret_cast<const std::byte*>(text.data()), text.size()}, net.target));
        EXPECT_TRUE(tx.flush(s).has_value());

        for (int attempt{}; attempt < 100; ++attempt) {
            if (const auto got{rx.receive(net.receiver)}; got && *got > 0) {
                return dgram::route(rx, dgram::by_peer{}, table, 0ns, unmatched, context...);
            }
        }
        ADD_FAILURE() << "nothing arrived on the loopback";
        return {};
    }
};

TEST(Route, AnUnmatchedCallbackThatClaimsADatagramIsCountedApart) {
    routing_fixture net{};
    auto rx{rx_batch::carve(net.net.arena)};
    auto tx{tx_batch::carve(net.net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    using rec_table = dgram::flow_table<dgram::peer_key, recorder*, 64>;
    auto table{rec_table::carve(net.table_arena)};
    ASSERT_TRUE(table.has_value());

    auto stranger{dgram::socket::open<>(dgram::family::inet4)};
    ASSERT_TRUE(stranger.has_value());
    ASSERT_TRUE(stranger->bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());

    recorder accepted_sink{};
    const auto counts{net.exchange(*rx, *tx, *stranger, "hello", *table, [&](const auto& a, const auto& key) {
        if (!key) {
            return false;
        }
        EXPECT_NE(table->insert(*key, &accepted_sink), nullptr);
        accepted_sink.on_datagram(a);
        return true;
    })};

    EXPECT_EQ(counts.accepted, 1u) << "a claimed datagram reached a protocol and must not read as junk";
    EXPECT_EQ(counts.unmatched, 0u);
    EXPECT_EQ(counts.delivered, 0u) << "it was not delivered to an existing flow either";
    EXPECT_EQ(counts.handled(), 1u);
    EXPECT_EQ(accepted_sink.seen, (std::vector<std::string>{"hello"}));
}

TEST(Route, AnUnmatchedCallbackThatDeclinesIsStillUnmatched) {
    routing_fixture net{};
    auto rx{rx_batch::carve(net.net.arena)};
    auto tx{tx_batch::carve(net.net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    using rec_table = dgram::flow_table<dgram::peer_key, recorder*, 64>;
    auto table{rec_table::carve(net.table_arena)};
    ASSERT_TRUE(table.has_value());

    auto stranger{dgram::socket::open<>(dgram::family::inet4)};
    ASSERT_TRUE(stranger.has_value());
    ASSERT_TRUE(stranger->bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());

    std::size_t offered{};
    const auto counts{net.exchange(*rx, *tx, *stranger, "junk", *table, [&](const auto&, const auto&) {
        ++offered;
        return false;
    })};
    EXPECT_EQ(offered, 1u);
    EXPECT_EQ(counts.accepted, 0u);
    EXPECT_EQ(counts.unmatched, 1u);
}

/* A callback returning void is what every caller wrote before `accepted`
   existed, and it must keep meaning "I did not take it". */
TEST(Route, AVoidUnmatchedCallbackNeverClaimsADatagram) {
    routing_fixture net{};
    auto rx{rx_batch::carve(net.net.arena)};
    auto tx{tx_batch::carve(net.net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    using rec_table = dgram::flow_table<dgram::peer_key, recorder*, 64>;
    auto table{rec_table::carve(net.table_arena)};
    ASSERT_TRUE(table.has_value());

    auto stranger{dgram::socket::open<>(dgram::family::inet4)};
    ASSERT_TRUE(stranger.has_value());
    ASSERT_TRUE(stranger->bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());

    std::size_t offered{};
    const auto counts{net.exchange(*rx, *tx, *stranger, "junk", *table, [&](const auto&, const auto&) { ++offered; })};
    EXPECT_EQ(offered, 1u);
    EXPECT_EQ(counts.unmatched, 1u);
    EXPECT_EQ(counts.accepted, 0u);
}

TEST(Route, ExtraArgumentsReachASinkThatTakesThem) {
    routing_fixture net{};
    auto rx{rx_batch::carve(net.net.arena)};
    auto tx{tx_batch::carve(net.net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    using fwd_table = dgram::flow_table<dgram::peer_key, forwarding_recorder*, 64>;
    libmem::arena table_arena{fwd_table::footprint()};
    auto table{fwd_table::carve(table_arena)};
    ASSERT_TRUE(table.has_value());

    auto known{dgram::socket::open<>(dgram::family::inet4)};
    ASSERT_TRUE(known.has_value());
    ASSERT_TRUE(known->bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());

    forwarding_recorder sink{};
    ASSERT_NE(table->insert(dgram::peer_key{ep("127.0.0.1", known->local_address()->port())}, &sink), nullptr);

    std::vector<std::string> landed{};
    const auto counts{net.exchange(*rx, *tx, *known, "context", *table, [](const auto&, const auto&) { return false; }, landed)};
    EXPECT_EQ(counts.delivered, 1u);
    EXPECT_EQ(landed, (std::vector<std::string>{"context"})) << "the sink took what route was given, without holding it";
}

/* The fallback: passing context to a sink that does not want it is not an
   error, so adding an argument at one call site cannot break another sink. */
TEST(Route, ASinkThatIgnoresExtraArgumentsStillCompiles) {
    routing_fixture net{};
    auto rx{rx_batch::carve(net.net.arena)};
    auto tx{tx_batch::carve(net.net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    using rec_table = dgram::flow_table<dgram::peer_key, recorder*, 64>;
    auto table{rec_table::carve(net.table_arena)};
    ASSERT_TRUE(table.has_value());

    auto known{dgram::socket::open<>(dgram::family::inet4)};
    ASSERT_TRUE(known.has_value());
    ASSERT_TRUE(known->bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());

    recorder sink{};
    ASSERT_NE(table->insert(dgram::peer_key{ep("127.0.0.1", known->local_address()->port())}, &sink), nullptr);

    std::vector<std::string> ignored{};
    const auto counts{net.exchange(*rx, *tx, *known, "plain", *table, [](const auto&, const auto&) { return false; }, ignored)};
    EXPECT_EQ(counts.delivered, 1u);
    EXPECT_TRUE(ignored.empty());
    EXPECT_EQ(sink.seen, (std::vector<std::string>{"plain"}));
}

/* Coalescing is invisible to a receive loop by design, which also means a caller
   paying for 64 KiB slots has no way to tell whether it is getting anything for
   them. `slots` is that way. */
TEST(Route, ReportsHowManySlotsTheDatagramsCameIn) {
    fixture net{};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    auto sender{dgram::socket::open<>(dgram::family::inet4)};
    ASSERT_TRUE(sender.has_value());
    ASSERT_TRUE(sender->bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());

    using rec_table = dgram::flow_table<dgram::peer_key, recorder*, 64>;
    libmem::arena table_arena{rec_table::footprint()};
    auto table{rec_table::carve(table_arena)};
    ASSERT_TRUE(table.has_value());

    recorder sink{};
    ASSERT_NE(table->insert(dgram::peer_key{ep("127.0.0.1", sender->local_address()->port())}, &sink), nullptr);

    // One send, segmented into four: the kernel may coalesce them back or not.
    constexpr std::uint16_t stride{100};
    std::vector<std::byte> payload(4 * stride, std::byte{'x'});
    dgram::control<tx_set> ancillary{};
    (void)ancillary.set<dgram::segment>(stride);
    ASSERT_TRUE(tx->stage_copy(payload, net.target, ancillary));
    ASSERT_TRUE(tx->flush(*sender).has_value());

    dgram::routed counts{};
    for (int attempt{}; attempt < 100 && counts.handled() < 4; ++attempt) {
        if (const auto got{rx->receive(net.receiver)}; got && *got > 0) {
            const auto pass{dgram::route(*rx, dgram::by_peer{}, *table, 0ns)};
            counts.slots += pass.slots;
            counts.delivered += pass.delivered;
        }
    }

    ASSERT_EQ(counts.handled(), 4u);
    EXPECT_GT(counts.slots, 0u);
    EXPECT_LE(counts.slots, 4u);
    EXPECT_GE(counts.per_slot(), 1.0) << "every slot walked yielded at least one datagram";
}

} // namespace
