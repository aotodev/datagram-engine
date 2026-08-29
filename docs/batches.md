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
for (const auto& d : view | std::views::filter(&dgram::datagram::intact)) { ... }
```

**A view from a previous `receive` dangles once the next one is issued.** The
whole block is reused in place.

## `datagram`

One received datagram, borrowed from the batch that received it.

| Member | Description |
|--------|-------------|
| `std::span<const std::byte> payload()` | The bytes. Valid until the next `receive`. |
| `const endpoint& from()` | Sender address, referencing the batch's own storage. |
| `int flags()` | Raw `msg_flags`. |
| `bool truncated()` | `MSG_TRUNC`: the payload did not fit `SlotBytes` and the excess is gone. |
| `bool control_truncated()` | `MSG_CTRUNC`: ancillary data did not fit and some is gone. |
| `bool intact()` | Neither. |

**Check `intact()`.** The kernel reports truncation no other way, so an
undersized `SlotBytes` or control buffer otherwise reads as a short but valid
datagram.

## Transmitting

```cpp
bool stage(std::span<const std::byte> payload, const endpoint& to);
bool stage_copy(std::span<const std::byte> payload, const endpoint& to);  // SlotBytes > 0
result<std::size_t> flush(const socket& sock, int flags = 0);
bool full() const;
std::size_t staged() const;
```

`stage` references the caller's bytes rather than copying, so bouncing a datagram
straight out of a receive slot costs nothing. **The referenced bytes must stay
valid until `flush` returns.** It yields `false` when the batch is full.

`stage_copy` copies into the batch's own slot, for bytes that will not outlive
the flush. It requires `SlotBytes > 0` and yields `false` if the payload exceeds
it.

`flush` sends everything staged and clears the batch, returning how many were
taken. A short send is not an error and the remainder is dropped: datagram
delivery is unreliable by definition, so requeueing buys nothing a protocol layer
cannot do better.

## Retaining bytes past the batch

There is no retention mechanism. A protocol that reassembles or retransmits, as
QUIC does, copies what it needs out of the payload before the next `receive`.
