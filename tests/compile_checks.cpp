// Pure compile-time checks over the geometry and feature-set layer. Never run.

import std;
import libmem;
import dgram;

namespace {

struct fake_pktinfo {
    static constexpr std::size_t space{32};
};
struct fake_ecn {
    static constexpr std::size_t space{16};
};

static_assert(dgram::cmsg_feature<fake_pktinfo>);
static_assert(dgram::feature_set<dgram::no_features>);

using two = dgram::features<fake_pktinfo, fake_ecn>;
static_assert(two::count == 2);
static_assert(two::control_space == 48, "control space is the sum over the enabled set");
static_assert(two::contains<fake_pktinfo>);
static_assert(!two::contains<int>);

// Order must not change the size.
static_assert(dgram::features<fake_ecn, fake_pktinfo>::control_space == two::control_space);

// Footprint grows with every dimension and is never zero.
constexpr dgram::layout small{.capacity = 8, .slot_bytes = 1024, .control_bytes = 0};
constexpr dgram::layout wide{.capacity = 64, .slot_bytes = 1024, .control_bytes = 0};
constexpr dgram::layout deep{.capacity = 8, .slot_bytes = 2048, .control_bytes = 0};
constexpr dgram::layout with_cmsg{.capacity = 8, .slot_bytes = 1024, .control_bytes = 48};

static_assert(small.bytes() > small.capacity * small.slot_bytes, "footprint must cover the headers too");
static_assert(wide.bytes() > small.bytes());
static_assert(deep.bytes() > small.bytes());
static_assert(with_cmsg.bytes() > small.bytes());
static_assert(with_cmsg.bytes() - small.bytes() >= 8 * 48, "each datagram gets its own control block");

// A batch reports the same geometry it was parameterised with.
using rx = dgram::receive_batch<32, 1500, two>;
static_assert(rx::geometry.capacity == 32);
static_assert(rx::geometry.slot_bytes == 1500);
static_assert(rx::geometry.control_bytes == 48);
static_assert(rx::footprint() == rx::geometry.bytes());

// A transmit batch that only references costs no payload memory.
using tx_ref = dgram::transmit_batch<32>;
using tx_own = dgram::transmit_batch<32, 1500>;
static_assert(tx_ref::footprint() < tx_own::footprint());

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

} // namespace
