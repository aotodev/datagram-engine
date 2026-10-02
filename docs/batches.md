# Batches

A batch is a `recvmmsg` or `sendmmsg` argument block carved from a resource once,
before the loop starts, and reused for the lifetime of the program.

## Sizing and carving

```cpp
template <std::size_t Capacity, std::size_t SlotBytes, feature_set Features = no_features>
class receive_batch;

template <std::size_t Capacity, std::size_t SlotBytes = 0, feature_set Features = no_features>
class transmit_batch;
```

`Capacity` is datagrams per syscall and must be in `(0, IOV_MAX]`. `SlotBytes` is
payload bytes per datagram; a receive batch requires it to be non-zero, a
transmit batch treats zero as "reference only, carve no payload memory".
`Features` is the ancillary-data set, which decides the control-buffer size. See
[metadata](metadata.md).

| Member | Description |
|--------|-------------|
| `static constexpr layout geometry` | The `{capacity, slot_bytes, control_bytes}` the type was parameterised with. |
| `static constexpr std::size_t footprint()` | Bytes the batch draws from a resource. |
| `static result<batch> carve(R& resource)` | Take the memory and wire the headers. `out_of_memory` if the resource is exhausted. |

`footprint()` is an upper bound: every block is costed with its worst-case
leading padding, so an arena sized from it can never come up short. The slack is
a few dozen bytes.

```cpp
using rx = dgram::receive_batch<64, 2048>;
using tx = dgram::transmit_batch<64>;

libmem::arena arena{rx::footprint() + tx::footprint()};
auto receive{rx::carve(arena)};
auto transmit{tx::carve(arena)};
```

`R` must model `libmem::aligned_monotonic_resource`: the batch borrows and never
calls `deallocate`, so the resource has to reclaim everything at once. `arena`,
`typed_arena` and `resource_ref` to either all qualify, and a caller's own bump
allocator opts in with one specialisation of
`libmem::enable_monotonic_resource`. A resource that expects paired frees, such
as `libmem::default_resource`, is rejected at compile time rather than leaking.

The resource must outlive the batch.

## Receiving

```cpp
result<std::size_t> receive(const socket& sock, int flags = 0);
auto datagrams() const;        // lazy view of datagram
std::size_t received() const;
std::span<std::byte> slot(std::size_t i);
```

`receive` returns how many datagrams arrived. `MSG_WAITFORONE` is always set, so
the call returns as soon as one datagram is available and takes whatever else is
already queued; without it a blocking `recvmmsg` waits for all `Capacity`
messages.

On a `nonblocking` socket with nothing queued, the result is `would_block`.

`datagrams()` is a lazy view over what the last `receive` produced. It is not a
borrowed range, so bind it to a name before iterating:

```cpp
const auto view{batch.datagrams()};
for (const auto& d : view | std::views::filter(dgram::is_intact)) { ... }
```

`is_intact`, `is_truncated` and `is_control_truncated` are stateless predicates
usable as adaptors. Prefer them to a member pointer: `datagram` is templated on
the feature set, so `&datagram<Features>::intact` would name that set at every
call site.

**A view from a previous `receive` dangles once the next one is issued.** The
whole block is reused in place.

## `datagram`

One received datagram, borrowed from the batch that received it.

| Member | Description |
|--------|-------------|
| `std::span<const std::byte> payload()` | The bytes. Valid until the next `receive`. |
| `const endpoint& from()` | Sender address, referencing the batch's own storage. |
| `int flags()` | Raw `msg_flags`. |
| `metadata<Features> meta()` | Parse this datagram's ancillary data. See [metadata](metadata.md). |
| `auto segments()` | Lazy view of the datagrams packed into this slot. One element unless the kernel coalesced. See [offload](offload.md). |
| `bool truncated()` | `MSG_TRUNC`: the payload did not fit `SlotBytes` and the excess is gone. |
| `bool control_truncated()` | `MSG_CTRUNC`: ancillary data did not fit and some is gone. |
| `bool intact()` | Neither. |

**Check `intact()`.** The kernel reports truncation no other way, so an
undersized `SlotBytes` or control buffer otherwise reads as a short but valid
datagram.

## Transmitting

```cpp
bool stage(std::span<const std::byte> payload, const endpoint& to);
bool stage(std::span<const std::byte> payload, const endpoint& to, const control<Features>&);
bool stage_copy(std::span<const std::byte> payload, const endpoint& to,
                const control<Features>& = {});                          // SlotBytes > 0
flushed flush(const socket& sock, int flags = 0);
void discard();
bool full() const;
std::size_t staged() const;
```

`stage` references the caller's bytes rather than copying, so bouncing a datagram
straight out of a receive slot costs nothing. **The referenced bytes must stay
valid until a flush sends them or `discard()` drops them.** It yields `false`
when the batch is full.

`stage_copy` copies into the batch's own slot, for bytes that will not outlive
the flush. It requires `SlotBytes > 0` and yields `false` if the payload exceeds
it.

The `control` overload attaches ancillary data to that one datagram; see
[metadata](metadata.md). `msg_controllen` is set from what was actually built,
zero included, so nothing carries over between datagrams staged in the same slot.

`flush` sends everything staged and reports what happened:

| `flushed` member | Description |
|------------------|-------------|
| `sent` | Datagrams the kernel took. |
| `rejected` | Entries dropped because the kernel refused that datagram. |
| `last_rejection` | Why the last of those was refused. |
| `stalled` | Why the flush stopped early. Set means entries are still staged. |
| `drained()` | `stalled` is empty: every entry was sent or rejected. |

`sendmmsg` stops at the first datagram it cannot send, so `flush` decides who the
error belongs to.

- **The datagram's own** (`EMSGSIZE`, `EHOSTUNREACH`, `EPERM` from a firewall,
  `EACCES` on broadcast, ...): that entry is dropped and the rest are still sent.
  One bad destination cannot hold up everyone else in the batch, and a
  per-destination error that repeats cannot wedge it.
- **The socket's** (`EAGAIN`, `ENOBUFS`, `ENOMEM`, `EINTR`, `EBADF`, `ENOTSOCK`,
  `EOPNOTSUPP`): the next entry would hit it too, so `flush` stops. Everything
  not yet sent stays staged, in order, including the tail of a send that got
  partway. Flush again once the socket is writable, or `discard()` it.

**Entries a stalled flush keeps still reference the caller's bytes.** If those
are receive slots, the next `receive` overwrites them, and a later flush would
send new datagrams' bytes to old datagrams' peers. Flush again before receiving,
or discard:

```cpp
const auto sent{tx->flush(*sock)};
if (sent.stalled == dgram::would_block) {
    wait_until_writable(*sock);  // then flush again, before the next receive
} else if (sent.stalled) {
    tx->discard();
}
```

Stalled entries keep their slots until they go out, so `full()` can be true with
fewer than `Capacity` staged.

## Retaining bytes past the batch

There is no retention mechanism. A protocol that reassembles or retransmits, as
QUIC does, copies what it needs out of the payload before the next `receive`.

There is one escape hatch, and it does not reach a protocol.
`receive_batch::slot(i)` hands out the raw slot, but only a caller walking
`datagrams()` itself can use it: an `arrival` carries no slot index, so a
protocol reached through [`route`](demux.md) cannot name the slot its bytes came
from and has to copy.

**What that copy costs was measured rather than assumed.** For a go-back-N
protocol over `netem`, 100 datagrams through a send window of eight with a
reorder buffer of four:

| link | copied / delivered |
|------|--------------------|
| loopback, or `fq` | 0% |
| `loss 10% delay 5ms` | 20% to 32% |
| the same plus `reorder 25% 50%` | 20% to 35% |

The governing quantity is `min(Window - 1, Reorder)`, not the loss rate: a
sender does not stop at a gap, so everything behind a lost datagram arrives
early and has to be held, and a receiver holds at most `Reorder` of it before
refusing the rest. A protocol that delivers in order and drops what it cannot
use immediately copies nothing at all, and one with no reorder buffer trades the
copy for retransmissions instead.

So retention is one side of a trade rather than a cost the engine imposes, and
it pays only where the *reorder* buffer is large. That argues for an opt-in
per-batch retention API if one is ever built, not for widening the borrow
contract every caller pays for.
