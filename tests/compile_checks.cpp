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

} // namespace
