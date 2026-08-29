# Segmentation offload

Both halves move the per-datagram framing loop out of user space. `UDP_SEGMENT`
lets one `sendmmsg` entry carry a buffer the kernel slices; `UDP_GRO` lets one
`recvmmsg` slot come back holding several datagrams' worth of bytes.

## Directions are separate feature sets

`UDP_GRO` is only ever received and `UDP_SEGMENT` only ever sent, so they are not
duals of one feature and do not belong in one set. A receive and a transmit batch
each take their own:

```cpp
using rx_features = dgram::features<dgram::pktinfo, dgram::ecn, dgram::gro>;
using tx_features = dgram::features<dgram::ecn, dgram::segment>;

using rx = dgram::receive_batch<16, 1 << 16, rx_features>;
using tx = dgram::transmit_batch<16, 0, tx_features>;
```

`gro` models `receivable_feature` but not `sendable_feature`; `segment` is the
reverse. The concepts enforce it, so putting `segment` in a receive set and
expecting it to parse does not compile.

## Receiving: `gro`

Enable it like any other feature, through `receive_metadata`. Its value is the
size every datagram in the buffer had **except possibly the last**, which is
whatever remained.

The kernel omits the control message entirely when it did not coalesce, so
absent means "this slot holds exactly one datagram", not "reporting was off".

**Size the slot for the coalesced buffer, not for one MTU.** This is the mistake
that makes GRO look broken: a slot of 2048 bytes receiving a coalesced 60 KB
buffer reports `MSG_TRUNC` on every datagram and throws away most of the data.
64 KB is the practical ceiling, so a GRO batch trades capacity for slot size.

```cpp
constexpr std::size_t slot_bytes{1 << 16};   // not an MTU
```

## Walking a slot: `segments()`

```cpp
for (const auto& d : view | std::views::filter(dgram::is_intact)) {
    for (const auto& piece : d.segments()) {
        (void)tx.stage(piece, d.from(), reply);
    }
}
```

`segments()` yields the datagrams packed into the slot. When nothing was
coalesced it yields the whole payload as a single element, so **a receive loop
never has to branch on whether offload is in play**.

It is a lazy view of `std::span<const std::byte>`, random-access and trivially
copyable, over the slot's own memory. Deliberately not a `std::generator`: this
is the innermost loop of the receive path and a generator would heap-allocate its
frame on every slot.

`segments()` parses the control buffer to find the stride. A caller that already
holds a `meta()` should skip the second walk:

```cpp
const auto meta{d.meta()};
const auto stride{meta.get<dgram::gro>().value_or(0)};
for (const auto& piece : dgram::segments_of(d.payload(), stride)) { ... }
```

`segments_of(bytes, stride)` is the free function underneath. A `stride` of zero,
or one at least as large as the buffer, yields the whole buffer as one datagram.
An empty payload yields one empty datagram, because a zero-length UDP datagram is
legal and dropping it would lose it silently.

## Sending: `segment`

Per datagram rather than per socket, so one batch can mix segmented and ordinary
entries:

```cpp
dgram::control<tx_features> ancillary{};
ancillary.set<dgram::segment>(1400);
(void)tx.stage(one_large_buffer, peer, ancillary);
```

The buffer may exceed the MTU by a wide margin; the kernel or the NIC slices it
at the given size, and the final slice is whatever remains. The value is a
`uint16_t`, not the `int` that `UDP_GRO` reports back.

## What the kernel actually does

Measured on Linux 7.1, not recalled:

| Sent | `gso_size` reported | Slots | Segments |
|------|--------------------|-------|----------|
| 500 B, no `UDP_SEGMENT` | absent | 1 | 1 x 500 |
| 4200 B, segment 1400 | 1400 | 1 | 3 x 1400 |
| 4400 B, segment 1400 | 1400 | 1 | 3 x 1400 + 200 |
| 300 B, segment 1400 | absent | 1 | 1 x 300 |

The pattern to take from it: an exact multiple produces no empty trailing
segment, and a payload smaller than the segment size is simply not coalesced.
