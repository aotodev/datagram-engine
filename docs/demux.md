# Demultiplexing

The boundary the engine exists to reach. Above this line a protocol sees bytes,
an address and a time, and nothing about sockets, `msghdr` or file descriptors.

## No locking, deliberately

There is no atomic and no lock here. The engine's model is one socket, one arena
and one table per thread behind `SO_REUSEPORT`, so a flow table is never shared
between threads. A lock-free map would pay contention costs for sharing that
does not happen; plain open addressing is faster, not a compromise.

If you do want a shared table, substitute your own: `route` takes anything
satisfying `flow_lookup`.

## What a protocol receives

```cpp
template <feature_set Features>
struct arrival {
    std::span<const std::byte> payload;
    const endpoint& from;
    const metadata<Features>& meta;
    departure at;
};
```

`payload` is one datagram, already split out of a coalesced slot. An arrival is
a view, not a value: it holds references, so it cannot be assigned or reseated,
and the three borrows do not all last as long.

| member | valid until |
|--------|-------------|
| `payload` | the batch's next `receive` |
| `from` | the batch's next `receive` |
| `meta` | **the end of the `on_datagram` call** |

`meta` is the short one because `route` parses it once per slot into a local,
not once per datagram. A protocol that keeps anything copies it, and for `meta`
that means copying inside the call.

A sink is anything that can take one:

```cpp
struct my_protocol {
    void on_datagram(const dgram::arrival<my_features>& a);
};
```

That is the whole `datagram_sink` concept. No base class, no virtual, no
registration.

## Keys and projections

Which key identifies a flow is the caller's decision, because it is a protocol
question and the answers genuinely differ.

| Projection | Key | Use |
|------------|-----|-----|
| `by_peer` | peer address and port | A socket bound to one local address. |
| `by_flow` | the full 4-tuple | A wildcard-bound socket, where two peers can arrive on different local addresses. Requires `pktinfo`. |
| `by_payload_id<Offset, Length>` | bytes from the payload | A protocol identifier, such as a QUIC connection id. |

`by_payload_id` is why this is a projection rather than something the engine
picks. **A QUIC connection survives its peer changing address**, so the 4-tuple
is exactly the wrong key for it; the connection id at a fixed offset is right.

Every projection returns `std::optional`: no key is a normal outcome. A payload
too short to hold the identifier yields nothing rather than reading past the end,
and `by_flow` yields nothing when `pktinfo` was not enabled rather than silently
collapsing every local address into one flow. `by_flow` does not compile at all
against a feature set without `pktinfo`.

Writing your own is one call operator:

```cpp
struct by_first_byte {
    using key_type = dgram::byte_key<1>;
    template <typename Features>
    std::optional<key_type> operator()(const dgram::arrival<Features>& a) const noexcept {
        if (a.payload.empty()) { return std::nullopt; }
        return key_type{a.payload.first(1)};
    }
};
```

### Addresses as keys

`endpoint` compares and hashes on family, port, address bytes and IPv6 scope,
and nothing else. Not a `memcmp` of the whole `sockaddr`: that would include
`sin_zero` padding on v4 and `sin6_flowinfo` on v6. A flow label is not identity,
and comparing it would make the same peer look new on every datagram, filling a
table with duplicates. Scope *is* identity, since `fe80::1%eth0` and
`fe80::1%eth1` are different destinations.

## The table

```cpp
using table = dgram::flow_table<dgram::peer_key, connection*, 1024>;

libmem::arena arena{table::footprint()};
auto flows{table::carve(arena)};
```

Open addressing with linear probing and backward-shift deletion, carved from a
resource like everything else. `Slots` must be a power of two.

| Member | Description |
|--------|-------------|
| `max_size` | Flows it will hold: seven eighths of `Slots`. |
| `Value* find(key)` | The flow, or `nullptr`. |
| `Value* insert(key, value)` | Inserts or overwrites. Points at the stored value; `nullptr` means the table was full. |
| `bool erase(key)` | `false` if it was not there, which is not an error. Not `[[nodiscard]]`. |
| `auto entries()` | Lazy view of every live pair, for sweeps and timeouts. |

**The table cannot grow.** It is carved once, so `insert` refuses at `max_size`
rather than degrading: a nearly full open-addressed table turns every lookup
into a linear scan. Size `Slots` for the peak flow count you intend to serve and
treat a refusal as backpressure.

Backward-shift deletion means no tombstones, so a long-lived table that churns
connections does not slowly fill with dead markers.

Values must be trivially destructible, because the table never runs a
destructor. Store a pointer or an index into a pool, which is what protocol
state usually is anyway. `route` dereferences a pointer value automatically, so
both shapes work.

## Routing

```cpp
const auto counts{dgram::route(batch, dgram::by_peer{}, flows, now,
                               [&](const auto& a, const auto& key) { /* unknown flow */ })};
```

| Field of `routed` | Meaning |
|-------------------|---------|
| `slots` | receive slots walked, whether or not the kernel coalesced |
| `delivered` | handed to a flow that already existed |
| `accepted` | the `unmatched` callback claimed it, by returning `true` |
| `unmatched` | no key, no flow for it, and the callback did not claim it |
| `dropped` | arrived truncated, never delivered |
| `handled()` | `delivered + accepted`: everything that reached a protocol |
| `per_slot()` | datagrams per slot, which is what `gro` is buying |

`per_slot()` is the only way to tell whether coalescing is happening. A socket
with `gro` in its set needs a receive slot sized for a 64 KiB coalesced buffer
rather than an MTU, which is a large standing cost; if this stays at one, that
cost is being paid for nothing. A truncated slot counts as a slot and yields no
datagrams, so an undersized slot drives this below one rather than above it.

Size the slot against `gro::max_coalesced` rather than a literal:

```cpp
static_assert(SlotBytes >= dgram::gro::max_coalesced,
              "a GRO receive slot must hold a coalesced buffer, not one datagram");
```

That is a consumer's assert, not the engine's: an undersized GRO slot is a
legitimate thing to build, and truncation is reported rather than hidden.

**Routing is per datagram, not per slot.** With GRO a single slot can hold
datagrams belonging to different flows, and dispatching on the slot would hand
all of them to whichever flow the first one keyed to. Metadata is parsed once per
slot and shared, since ancillary data describes the slot rather than the
datagrams inside it.

Truncated datagrams are counted and never delivered: a protocol should not have
to ask whether the bytes it was handed are complete.

The `unmatched` callback is where a server decides whether to accept a new
connection. The overload without it discards unmatched datagrams.

**A callback that accepts one says so by returning `true`.** It still delivers
that first datagram itself, since it is the thing that just created the flow;
returning `true` only decides which counter moves. Without it a server that
accepts connections has permanently nonzero `unmatched` in steady state and
cannot tell "connections are being accepted" from "junk is arriving". A callback
returning `void` never claims a datagram, which is what the discarding overload
wants and what every caller written before this did.

## Handing something to the sink

`on_datagram(a)` is all the engine requires, and for a long time it was all the
engine would call, so anything a protocol needed at delivery time had to be
state it already held. Arguments after the `unmatched` callback are forwarded:

```cpp
const auto counts{dgram::route(batch, dgram::by_peer{}, flows, now, accept, application)};
```

```cpp
struct my_protocol {
    void on_datagram(const dgram::arrival<my_features>& a, my_app& app);
};
```

A sink whose `on_datagram` does not take them keeps being called with one
argument, so adding context at one call site cannot break a sink elsewhere.

## Timeouts

Flow state needs to expire. See [timers](timers.md) for the wheel, which pairs
with `flow_table` by carrying a key or an index as its payload.
