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

## Building

Needs GCC >= 16, CMake >= 3.30 and Ninja. Linux only.

```sh
cmake -S . -B build -G Ninja -DDGRAM_BUILD_TESTS=ON -DDGRAM_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

`libmem` is fetched automatically; point at a local checkout with
`-DFETCHCONTENT_SOURCE_DIR_LIBMEM=/path/to/libmem`.

## Status

Phase 1 (memory model and the batching syscalls) is done and tested. Phases 2 to
5, covering ancillary data, GRO/GSO, pacing and demultiplexing, are in progress.

## Docs

- [Sockets](docs/sockets.md): `socket`, `endpoint`, and the option set.
- [Batches](docs/batches.md): sizing, carving, receiving, transmitting, and the borrow contract.
- [Errors](docs/errors.md): `result`, `errc`, and the pipe combinators.
- [Integration](docs/integration.md): consuming dgram from another CMake project.
