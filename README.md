# datagram-engine

A Linux UDP datagram engine in C++26: batched send and receive, all memory
carved once at startup, and a protocol boundary that hands out nothing but
`std::span` and timing.

Built on `recvmmsg` / `sendmmsg`, ancillary data (`IP_PKTINFO`, DSCP and ECN), hardware
segmentation offload (`UDP_GRO` / `UDP_SEGMENT`) and transmit pacing
(`SO_TXTIME`). It is not a general network library and has no TCP: the whole
design turns on a batch being reclaimable in one step, which a byte stream is
not.

## At a glance

```cpp
import dgram;
import libmem;

using rx = dgram::receive_batch<64, 2048, dgram::features<dgram::ecn>>;
using tx = dgram::transmit_batch<64>;

auto sock = dgram::socket::open<dgram::reuse_port,
                                dgram::receive_metadata<dgram::ecn>>(dgram::family::inet4);

// Setup is a chain: each step runs only if the last succeeded, and the first
// error is the one that comes out.
const auto listening = sock->bind(dgram::endpoint::any(dgram::family::inet4, 9000))
                     | dgram::then([&] { return sock->local_address(); })
                     | dgram::map(&dgram::endpoint::text)
                     | dgram::recover([](dgram::errc e) {
                           return dgram::result<std::string>{std::string{dgram::describe(e)}};
                       });

libmem::arena arena{rx::footprint() + tx::footprint()};
auto in = rx::carve(arena);
auto out = tx::carve(arena);

// The loop is a range pipeline. `segments()` splits a GRO-coalesced slot, so
// nothing here branches on whether the kernel coalesced.
(void)in->receive(*sock);
for (const auto& d : in->datagrams() | std::views::filter(dgram::is_intact)) {
    for (const auto& piece : d.segments()) {
        (void)out->stage(piece, d.from());
    }
}
(void)out->flush(*sock);   // a refused datagram is dropped; the rest still go
out->discard();            // whatever a stalled flush kept still points into `in`
```

`then`, `map`, `recover` and `tap` compose a `result` left to right, in the order
the steps run. They wrap `std::expected`'s `and_then` / `transform` / `or_else`,
which read right to left and nest. Nothing throws, and a failure short-circuits
the rest of the chain.

The full program is [examples/echo.cpp](examples/echo.cpp): a batched echo server
that reflects each datagram's ECN marking, reports the local address it arrived
on, and splits coalesced slots, in about 90 lines. Every snippet on this page is
compiled and run as a test, so none of them can rot.

## What's here

| Component | Description |
|-----------|-------------|
| `socket` | Move-only UDP descriptor. Options are types, composed at the call site and applied in order. |
| `endpoint` | IPv4 / IPv6 address in the kernel's own layout, so the receive path never converts. |
| `receive_batch` | `recvmmsg` argument block carved from a resource, plus a lazy view over what arrived. |
| `transmit_batch` | `sendmmsg` argument block. Stages by reference, so echoing costs no copy. |
| `features` | Compile-time set of ancillary-data features; the control buffer is sized as the sum over it. |
| `pktinfo` / `ecn` / `traffic_class` | Which local address a datagram arrived on, and its ECN and DSCP marking. Both families. |
| `gro` / `segment` | Hardware segmentation offload: many datagrams per slot in, one large buffer out. |
| `txtime` / `pacer` | Per-datagram departure times via `SO_TXTIME`, and drift-free rate arithmetic. |
| `flow_table` / `route` | Open-addressing demultiplexing to protocol state, keyed by a caller-supplied projection and hashed under a per-table SipHash seed, so peers cannot flood it. |
| `timer_wheel` | Hierarchical timing wheel for protocol timeouts: constant-time schedule, cancel and expiry. |
| `result` | `std::expected` over `errno`, with left-to-right pipe combinators. |

## What makes it different

**Every allocation happens before the loop starts.** A batch reports a
`constexpr footprint()`, the caller sizes one arena from it, and `carve` takes
the whole block in one pass. Nothing on the receive or transmit path allocates
after that: the echo example measures zero allocations across 300 round trips,
counted at every glibc entry point (`malloc`, `calloc`, `realloc`,
`aligned_alloc`, `posix_memalign`).

```cpp
using rx = dgram::receive_batch<64, 2048>;
using tx = dgram::transmit_batch<64>;

libmem::arena arena{rx::footprint() + tx::footprint()};
auto batch{rx::carve(arena)};
```

**Losing data is a return value, not a surprise.** `MSG_TRUNC` and `MSG_CTRUNC`
are surfaced per datagram rather than swallowed, so an undersized slot or control
buffer shows up as `truncated()` instead of silent corruption.

```cpp
for (const auto& d : batch->datagrams() | std::views::filter(dgram::is_intact)) {
    (void)out->stage(d.payload(), d.from());
}
```

**The metadata layer is a compile-time set.** Each feature contributes its
`CMSG_SPACE` term, its parse step and its build step from one declaration, so the
control buffer cannot fall out of step with what the parser expects.

```cpp
using rx_set = dgram::features<dgram::pktinfo, dgram::ecn>;
using tx_set = dgram::features<dgram::pktinfo, dgram::traffic_class>;

const auto meta{d.meta()};
if (const auto marking{meta.get<dgram::ecn>()}) { reply.set<dgram::traffic_class>({.ecn = *marking}); }
```

## Building

Needs GCC >= 16, CMake >= 3.30 and Ninja. Linux only.

```sh
scripts/make.sh                          # Debug build + tests
scripts/make.sh -s address+undefined -H  # sanitized and hardened
scripts/make.sh -a                       # every defensive configuration
```

Or directly with CMake:

```sh
cmake -S . -B build -G Ninja -DDGRAM_BUILD_TESTS=ON -DDGRAM_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

`libmem` is fetched automatically; point at a local checkout with
`-DFETCHCONTENT_SOURCE_DIR_LIBMEM=/path/to/libmem`.

## Tested and fuzzed

Every configuration builds and runs the suite: plain, **ASan + UBSan**,
**ThreadSanitizer**, and a **hardened** build with `_GLIBCXX_ASSERTIONS` and
stack protection. The concurrency suite exists specifically to put the
shared-nothing threading claim under a race detector.

The ancillary-data parser and the segmentation walk are fuzzed with **AFL++**,
since those are the places kernel-supplied values drive pointer arithmetic.
Everything the fuzzers have found is pinned as a unit test. Details in
[docs/testing.md](docs/testing.md).

**Offload is invisible to the receive loop.** A GRO slot holding several
datagrams and a slot holding one are iterated the same way, so nothing branches
on whether the kernel coalesced.

```cpp
for (const auto& piece : d.segments()) { (void)tx->stage(piece, d.from(), reply); }
```

**Protocol code never sees a socket.** A sink takes an `arrival`: bytes, a peer,
parsed metadata, a time. Which key identifies a flow is the caller's, because a
QUIC connection survives its peer changing address and the 4-tuple does not.

```cpp
const auto counts{dgram::route(batch, dgram::by_payload_id<1, 8>{}, flows, now)};
```

## Status

All five phases are done and tested: the memory model, the batching syscalls,
ancillary data (destination address, DSCP and ECN, both families), segmentation
offload (`UDP_GRO` / `UDP_SEGMENT`), transmit pacing (`SO_TXTIME`),
demultiplexing, and protocol timers.

Pacing needs `tc qdisc add dev <iface> root fq` on the interface or the kernel
ignores every departure time without reporting anything; see
[docs/pacing.md](docs/pacing.md).

## Docs

- [Sockets](docs/sockets.md): `socket`, `endpoint`, and the option set.
- [Batches](docs/batches.md): sizing, carving, receiving, transmitting, and the borrow contract.
- [Ancillary data](docs/metadata.md): the feature set, destination address, ECN, and writing a feature.
- [Offload](docs/offload.md): GRO and GSO, slot sizing, and the segmentation view.
- [Pacing](docs/pacing.md): `SO_TXTIME`, the `fq` precondition, and the rate arithmetic.
- [Demultiplexing](docs/demux.md): the protocol boundary, key projections, the flow table, and routing.
- [Timers](docs/timers.md): the timing wheel, handles, and what it refuses.
- [Errors](docs/errors.md): `result`, `errc`, and the pipe combinators.
- [Integration](docs/integration.md): consuming dgram from another CMake project.
- [Testing](docs/testing.md): the build matrix, what each suite covers, and how the fuzzing works.

## License

MIT, see [LICENSE](LICENSE).
