# datagram_engine

A Linux UDP datagram engine in C++26: batched send and receive, all memory
carved once at startup, and a protocol boundary that hands out nothing but
`std::span` and timing.

Built on `recvmmsg` / `sendmmsg`, ancillary data (`IP_PKTINFO`, ECN), hardware
segmentation offload (`UDP_GRO` / `UDP_SEGMENT`) and transmit pacing
(`SO_TXTIME`). It is not a general network library and has no TCP: the whole
design turns on a batch being reclaimable in one step, which a byte stream is
not.

## What's here

| Component | Description |
|-----------|-------------|
| `socket` | Move-only UDP descriptor. Options are types, composed at the call site and applied in order. |
| `endpoint` | IPv4 / IPv6 address in the kernel's own layout, so the receive path never converts. |
| `receive_batch` | `recvmmsg` argument block carved from a resource, plus a lazy view over what arrived. |
| `transmit_batch` | `sendmmsg` argument block. Stages by reference, so echoing costs no copy. |
| `features` | Compile-time set of ancillary-data features; the control buffer is sized as the sum over it. |
| `pktinfo` / `ecn` | Which local address a datagram arrived on, and its ECN marking. Both families. |
| `gro` / `segment` | Hardware segmentation offload: many datagrams per slot in, one large buffer out. |
| `txtime` / `pacer` | Per-datagram departure times via `SO_TXTIME`, and drift-free rate arithmetic. |
| `flow_table` / `route` | Open-addressing demultiplexing to protocol state, keyed by a caller-supplied projection. |
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
for (const auto& d : batch->datagrams() | std::views::filter(&dgram::datagram::intact)) {
    (void)out->stage(d.payload(), d.from());
}
```

**The metadata layer is a compile-time set.** Each feature contributes its
`CMSG_SPACE` term, its parse step and its build step from one declaration, so the
control buffer cannot fall out of step with what the parser expects.

```cpp
using metadata_set = dgram::features<dgram::pktinfo, dgram::ecn>;

const auto meta{d.meta()};
if (const auto marking{meta.get<dgram::ecn>()}) { reply.set<dgram::ecn>(*marking); }
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
ancillary data (destination address and ECN, both families), segmentation
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
