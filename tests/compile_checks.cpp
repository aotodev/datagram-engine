// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
// Pure compile-time checks over the geometry and feature-set layer. Never run.

#include <netinet/in.h>

import std;
import libmem;
import dgram;

namespace {

/* ============================================================================
 * The feature concepts
 * ============================================================================ */

static_assert(dgram::cmsg_feature<dgram::pktinfo>);
static_assert(dgram::cmsg_feature<dgram::ecn>);
static_assert(dgram::sendable_feature<dgram::pktinfo>);
static_assert(dgram::sendable_feature<dgram::ecn>);
static_assert(dgram::receivable_feature<dgram::pktinfo>);
static_assert(dgram::receivable_feature<dgram::ecn>);

/* A type carrying only a size is not a feature: it can contribute to the buffer
   but never be parsed out of it, which is exactly the mismatch to prevent. */
struct size_only {
    static constexpr std::size_t space{16};
};
static_assert(!dgram::cmsg_feature<size_only>);

/* ============================================================================
 * Control-buffer sizing
 * ============================================================================ */

using both = dgram::features<dgram::pktinfo, dgram::ecn>;
using only_ecn = dgram::features<dgram::ecn>;

static_assert(dgram::feature_set<both>);
static_assert(both::count == 2);
static_assert(both::contains<dgram::pktinfo> && both::contains<dgram::ecn>);
static_assert(!both::contains<int>);

/* The size is the sum over the set, derived from the features rather than
   restated, so adding a feature cannot leave the buffer behind. */
static_assert(both::control_space == dgram::pktinfo::space + dgram::ecn::space);
static_assert(only_ecn::control_space == dgram::ecn::space);
static_assert(dgram::no_features::control_space == 0);

/* Order must not change the size. */
static_assert(dgram::features<dgram::ecn, dgram::pktinfo>::control_space == both::control_space);

/* A feature present in both families is sized for the larger payload, because
   the receive path cannot know which will arrive. */
static_assert(dgram::pktinfo::space == std::max(dgram::detail::space_for(sizeof(::in_pktinfo)), dgram::detail::space_for(sizeof(::in6_pktinfo))));
static_assert(dgram::pktinfo::space >= dgram::detail::space_for(sizeof(::in6_pktinfo)));
static_assert(dgram::ecn::space >= dgram::detail::space_for(sizeof(int)));
static_assert(dgram::ecn::space >= dgram::detail::space_for(sizeof(std::uint8_t)));

/* CMSG_SPACE covers the header and the trailing pad, so it always exceeds the
   payload and is never less than CMSG_LEN. */
static_assert(dgram::detail::space_for(1) > 1);
static_assert(dgram::detail::space_for(20) >= dgram::detail::length_for(20));

/* Direction-specific features carry only the members their direction needs. */
static_assert(dgram::receivable_feature<dgram::gro> && !dgram::sendable_feature<dgram::gro>);
static_assert(dgram::sendable_feature<dgram::segment> && !dgram::parseable_feature<dgram::segment>);
static_assert(dgram::sendable_feature<dgram::txtime> && !dgram::parseable_feature<dgram::txtime>);

/* Four distinct cmsg payload widths are now in play, so every control-buffer
   term is derived from the feature rather than written down. */
using paced = dgram::features<dgram::txtime, dgram::ecn, dgram::segment>;
static_assert(paced::control_space == dgram::txtime::space + dgram::ecn::space + dgram::segment::space);
static_assert(dgram::txtime::space == dgram::detail::space_for(sizeof(std::uint64_t)));
static_assert(dgram::segment::space == dgram::detail::space_for(sizeof(std::uint16_t)));
static_assert(dgram::gro::space == dgram::detail::space_for(sizeof(int)));
/* CMSG_SPACE rounds the payload up to the cmsghdr alignment, so every payload
   from 1 to 8 bytes costs the same. Worth pinning: it means adding a small
   feature is often free in buffer terms, and it is not obvious from the name. */
static_assert(dgram::txtime::space == dgram::segment::space);
static_assert(dgram::segment::space == dgram::ecn::space);
static_assert(dgram::detail::space_for(1) == dgram::detail::space_for(8));
static_assert(dgram::detail::space_for(9) > dgram::detail::space_for(8));
static_assert(dgram::pktinfo::space > dgram::txtime::space, "a 20-byte payload does cost more");

/* The pacer is usable at compile time. */
static_assert([] {
    dgram::pacer p{1'000'000, std::chrono::nanoseconds{0}};
    (void)p.schedule(1000);
    return p.peek();
}() == std::chrono::milliseconds{1});

static_assert(dgram::socket_option<dgram::transmit_time<>>);
static_assert(dgram::socket_option<dgram::transmit_time<dgram::txtime_clock::tai, true, false>>);

/* ============================================================================
 * Batch geometry
 * ============================================================================ */

constexpr dgram::layout small{.capacity = 8, .slot_bytes = 1024, .control_bytes = 0};
constexpr dgram::layout wide{.capacity = 64, .slot_bytes = 1024, .control_bytes = 0};
constexpr dgram::layout deep{.capacity = 8, .slot_bytes = 2048, .control_bytes = 0};
constexpr dgram::layout with_cmsg{.capacity = 8, .slot_bytes = 1024, .control_bytes = both::control_space};

static_assert(small.bytes() > small.capacity * small.slot_bytes, "footprint must cover the headers too");
static_assert(wide.bytes() > small.bytes());
static_assert(deep.bytes() > small.bytes());
static_assert(with_cmsg.bytes() > small.bytes());
static_assert(with_cmsg.bytes() - small.bytes() >= with_cmsg.capacity * both::control_space, "each datagram gets its own control block");

/* A batch reports the geometry it was parameterised with, and the control term
   comes from the feature set rather than being passed separately. */
using rx = dgram::receive_batch<32, 1500, both>;
static_assert(rx::geometry.capacity == 32);
static_assert(rx::geometry.slot_bytes == 1500);
static_assert(rx::geometry.control_bytes == both::control_space);
static_assert(rx::footprint() == rx::geometry.bytes());
static_assert(rx::footprint() > dgram::receive_batch<32, 1500>::footprint(), "features cost control buffer");

/* A transmit batch that only references costs no payload memory. */
static_assert(dgram::transmit_batch<32>::footprint() < dgram::transmit_batch<32, 1500>::footprint());

/* ============================================================================
 * Resource requirements
 * ============================================================================ */

/* Carving borrows and never frees, so a resource that expects paired
   deallocation must not be accepted: it would leak every block. */
template <typename R>
concept carvable = requires(R& r) { dgram::receive_batch<8, 64>::carve(r); };

static_assert(carvable<libmem::arena>);
static_assert(carvable<libmem::typed_arena>);
static_assert(carvable<libmem::resource_ref<libmem::arena>>);
static_assert(!carvable<libmem::default_resource>, "operator new expects a paired delete");
static_assert(!carvable<libmem::resource_ref<libmem::default_resource>>);

/* allocator_resource is excluded a step earlier: it cannot express a runtime
   alignment, so it is deliberately not an aligned_memory_resource at all. */
static_assert(!libmem::aligned_memory_resource<libmem::allocator_resource<std::allocator<int>>>);
static_assert(!carvable<libmem::allocator_resource<std::allocator<int>>>);

/* ============================================================================
 * Metadata access is gated on the feature set
 * ============================================================================ */

template <typename Features, typename F>
concept readable = requires(const dgram::metadata<Features>& m) { m.template get<F>(); };

static_assert(readable<both, dgram::pktinfo>);
static_assert(readable<both, dgram::ecn>);
static_assert(!readable<only_ecn, dgram::pktinfo>, "a feature not in the set has no slot to read");
static_assert(!readable<dgram::no_features, dgram::ecn>);

/* `meta()` yields a temporary, so reading through it must hand back a value.
   A reference here would dangle at the end of the expression. */
using ecn_slot = std::optional<dgram::ecn_codepoint>;
static_assert(std::is_same_v<decltype(std::declval<const dgram::metadata<both>&>().get<dgram::ecn>()), const ecn_slot&>);
static_assert(std::is_same_v<decltype(std::declval<const dgram::metadata<both>>().get<dgram::ecn>()), ecn_slot>);

template <typename Features, typename F>
concept settable = requires(dgram::control<Features>& c, typename F::value_type v) { c.template set<F>(v); };

static_assert(settable<both, dgram::ecn>);
static_assert(!settable<only_ecn, dgram::pktinfo>);

/* The control buffer's size is carried by the type, not checked at runtime.
   `build_into` instantiates in the caller's translation unit, which need not
   have -fcontracts, so a precondition there would be silently compiled out. */
template <typename Set, std::size_t N>
concept buildable_into = requires(const dgram::control<Set>& c, std::span<std::byte, N> buf) { c.build_into(buf, dgram::family::inet4); };

static_assert(buildable_into<both, both::control_space>);
static_assert(!buildable_into<both, 4>, "an undersized control buffer must not compile");
static_assert(!buildable_into<both, std::dynamic_extent>, "an unsized control buffer must not compile either");
static_assert(!buildable_into<both, only_ecn::control_space>, "a buffer sized for a different set must not compile");

/* Socket options take the family, because the same intent is a different option
   per family. */
static_assert(dgram::socket_option<dgram::reuse_port>);
static_assert(dgram::socket_option<dgram::nonblocking>);
static_assert(dgram::socket_option<dgram::receive_metadata<dgram::pktinfo, dgram::ecn>>);

/* ============================================================================
 * Demultiplexing
 * ============================================================================ */

static_assert(dgram::demux_key<dgram::peer_key>);
static_assert(dgram::demux_key<dgram::flow_key>);
static_assert(dgram::demux_key<dgram::byte_key<20>>);

/* A key must be trivially destructible: the table never runs a destructor. */
static_assert(std::is_trivially_destructible_v<dgram::byte_key<20>>);
static_assert(std::is_trivially_destructible_v<dgram::flow_key>);

using demux_table_type = dgram::flow_table<dgram::peer_key, int, 64>;
static_assert(dgram::flow_lookup<demux_table_type, dgram::peer_key, int>);
static_assert(demux_table_type::max_size == 48, "three quarters of the slots, so probing stays bounded");
static_assert(demux_table_type::max_size < 64, "an open-addressed table must never be allowed to fill");

/* ============================================================================
 * Keyed hashing
 * ============================================================================ */

/* The reference key and messages: bytes 00 01 02 ... */
constexpr dgram::hash_seed reference_seed{0x0706050403020100ULL, 0x0F0E0D0C0B0A0908ULL};

template <int C, int D> consteval std::uint64_t sip_of_prefix(const std::size_t n, const std::size_t chunk = 64) {
    std::array<std::byte, 64> message{};
    for (std::size_t i{}; i < message.size(); ++i) {
        message[i] = static_cast<std::byte>(i);
    }
    dgram::basic_siphash<C, D> h{reference_seed};
    for (std::size_t at{}; at < n; at += chunk) {
        h(std::span<const std::byte>{message}.subspan(at, std::min(chunk, n - at)));
    }
    return h.finish();
}

/* The published SipHash-2-4 vectors pin the round function. */
static_assert(sip_of_prefix<2, 4>(0) == 0x726FDB47DD0E0E31ULL);
static_assert(sip_of_prefix<2, 4>(1) == 0x74F839C593DC67FDULL);
static_assert(sip_of_prefix<2, 4>(15) == 0xA129CA6149BE45E5ULL);

/* SipHash-1-3, the variant the table uses, around every word boundary. */
static_assert(dgram::siphash{reference_seed}.finish() == 0xABAC0158050FC4DCULL);
static_assert(sip_of_prefix<1, 3>(1) == 0xC9F49BF37D57CA93ULL);
static_assert(sip_of_prefix<1, 3>(7) == 0xD3927D989BB11140ULL);
static_assert(sip_of_prefix<1, 3>(8) == 0x369095118D299A8EULL);
static_assert(sip_of_prefix<1, 3>(9) == 0x25A48EB36C063DE4ULL);
static_assert(sip_of_prefix<1, 3>(15) == 0xD320D86D2A519956ULL);
static_assert(sip_of_prefix<1, 3>(16) == 0xCC4FDD1A7D908B66ULL);
static_assert(sip_of_prefix<1, 3>(63) == 0x9D199062B7BBB3A8ULL);

/* Streaming: how the bytes are split must not change the digest. */
static_assert(sip_of_prefix<1, 3>(63, 1) == sip_of_prefix<1, 3>(63));
static_assert(sip_of_prefix<1, 3>(63, 3) == sip_of_prefix<1, 3>(63));
static_assert(sip_of_prefix<1, 3>(63, 13) == sip_of_prefix<1, 3>(63));

/* A key hashed the old way, with only a hash_value, is not a demux key. */
struct unkeyed {
    int id{};
    friend bool operator==(const unkeyed&, const unkeyed&) = default;
    friend std::size_t hash_value(const unkeyed& k) noexcept { return static_cast<std::size_t>(k.id); }
};
static_assert(!dgram::demux_key<unkeyed>);

/* A sink is anything that can take an arrival, and nothing more. */
struct minimal_sink {
    void on_datagram(const dgram::arrival<dgram::no_features>&) {}
};
static_assert(dgram::datagram_sink<minimal_sink, dgram::no_features>);
struct not_a_sink {};
static_assert(!dgram::datagram_sink<not_a_sink, dgram::no_features>);

/* by_flow needs the local address, so it is only available when pktinfo is in
   the feature set. Keying on an unspecified local address would silently
   collapse every local address into one flow. */
template <typename Features>
concept flow_keyable = requires(const dgram::by_flow& p, const dgram::arrival<Features>& a) { p(a); };

static_assert(flow_keyable<dgram::features<dgram::pktinfo>>);
static_assert(!flow_keyable<dgram::no_features>, "the 4-tuple needs pktinfo");
static_assert(flow_keyable<both>);

/* by_peer and by_payload_id work with any feature set: neither reads metadata. */
template <typename Features, typename Projection>
concept projects = requires(const Projection& p, const dgram::arrival<Features>& a) { p(a); };

static_assert(projects<dgram::no_features, dgram::by_peer>);
static_assert(projects<dgram::no_features, dgram::by_payload_id<0, 8>>);

} // namespace
