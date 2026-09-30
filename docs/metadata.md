# Ancillary data

Datagrams carry more than payload and peer address. The kernel reports which
local address one arrived on, and what its DSCP and ECN marking were, as
*control messages* alongside the data. This layer turns those into typed values and
attaches them on the way out.

## The feature set

A feature is a type. The set of them is a compile-time list, and that one list
determines three things that must agree: how large the control buffer is, what
the parser looks for, and what can be attached on transmit.

```cpp
using rx_set = dgram::features<dgram::pktinfo, dgram::ecn>;
using tx_set = dgram::features<dgram::pktinfo, dgram::traffic_class>;

using rx = dgram::receive_batch<64, 2048, rx_set>;
using tx = dgram::transmit_batch<64, 0, tx_set>;
```

Not every feature goes both ways, so a receive and a transmit batch usually take
different sets. ECN is read with `ecn` and sent with `traffic_class`.

| Member | Description |
|--------|-------------|
| `count` | How many features. |
| `control_space` | Per-datagram control bytes, the sum of each feature's `space`. |
| `contains<F>` | Whether `F` is in the set. |

Sizing the buffer from the same list that drives parsing is the point. A
hand-maintained size that falls behind the parse list shows up as `MSG_CTRUNC`
and silently missing metadata, which is the failure this layer exists to
prevent. Repeating a feature is rejected at compile time.

## Enabling reporting

The kernel sends nothing until asked. `receive_metadata` is a socket option that
switches on every feature in the set:

```cpp
auto sock{dgram::socket::open<dgram::reuse_port,
                              dgram::receive_metadata<dgram::pktinfo, dgram::ecn>>(dgram::family::inet4)};
```

The family matters and is handled for you: the same intent is `IP_PKTINFO` on a
v4 socket and `IPV6_RECVPKTINFO` on a v6 one.

The set you enable and the set the batch is sized for should be the same one.
Enabling a feature the batch is not sized for costs `MSG_CTRUNC`; sizing for one
you never enable just wastes the bytes and reads as absent.

## Reading it

```cpp
const auto meta{d.meta()};
if (const auto& marking{meta.get<dgram::ecn>()}) { ... }
```

`meta()` walks that datagram's control buffer on each call rather than eagerly
after `receive`, so a caller that never asks pays nothing. Bind the result if
you need it more than once.

Every slot is independently absent. A feature whose option was never enabled, or
a datagram that carried no such header, yields nothing rather than a zeroed
value. Reading a feature that is not in the set does not compile.

The walk is bounded by `msg_controllen`, so a truncated control buffer yields a
short parse rather than a read past the end. The loss is reported separately, as
`control_truncated()`.

## Writing it

```cpp
dgram::control<tx_set> reply{};
reply.set<dgram::traffic_class>({.dscp = dgram::dscp::ef, .ecn = dgram::ecn_codepoint::ect0});
(void)tx.stage(payload, peer, reply);
```

Unset features contribute nothing. The control block is built per datagram and
`msg_controllen` set to exactly what was written, zero included, so a marking on
one datagram cannot leak onto the next one staged in the same slot.

The family for the build step is the destination's `wire_family()`, not the
socket's. A dual-stack socket sends to a v4-mapped peer down the kernel's IPv4
path, which ignores `IPV6_TCLASS` and every other v6-level message except
`IPV6_PKTINFO`, so those datagrams get `IP_TOS` and `IP_PKTINFO` instead.

## `pktinfo`

`IP_PKTINFO` / `IPV6_PKTINFO`. Value type is `local_info`:

| Field | Description |
|-------|-------------|
| `endpoint address` | The local address the datagram was sent to. Port is not populated. |
| `unsigned int interface` | Interface index, `0` when unspecified. |

The reason a wildcard-bound socket can answer from the address the peer actually
addressed, which matters the moment a host has more than one.

On transmit it selects the source address and interface. Leaving both zero lets
the routing table choose, which is the same as attaching nothing. A reply can
hand back the `local_info` it received unchanged, including the v4-mapped one a
dual-stack socket reports for an IPv4 peer.

Sized for the larger of the two families' payloads, because the receive path
cannot know which will arrive: a dual-stack socket reports `IPV6_PKTINFO` even
for v4-mapped traffic.

## `ecn` and `traffic_class`

Both read `IP_TOS` / `IPV6_TCLASS`, the traffic-class byte: six bits of DSCP
and two of ECN. `ecn` reduces it to the ECN bits and is **receive-only**.
`traffic_class` carries the whole byte as a `marking {dscp, ecn}` and goes both
ways.

`ecn` cannot be sent because the kernel takes a per-datagram traffic class as
the whole byte. Marking the ECN bits alone would zero the DSCP, so every
ECN-capable datagram would leave as best effort. Sending goes through
`traffic_class`, which makes the DSCP part of the value. That value also
replaces any DSCP the socket was given with `setsockopt(IP_TOS)`: a socket-level
DSCP only applies to datagrams that carry no `traffic_class`.

| `ecn_codepoint` | Meaning |
|-----------------|---------|
| `not_ect` | Not ECN-capable transport. |
| `ect1` | ECN-capable, codepoint 1. |
| `ect0` | ECN-capable, codepoint 0. |
| `ce` | Congestion experienced. |

`dscp` names the standard classes (`df`, `le`, `cs1` to `cs7`, `af11` to
`af43`, `voice_admit`, `ef`), and any value up to 63 is valid through a cast.

An unmarked datagram reads as `not_ect` (and `dscp::df`), not as absent,
provided reporting was enabled. A receive set can hold both features; they read
the same message.

On a v6 socket, enabling it sets both `IPV6_RECVTCLASS` and `IP_RECVTOS`. IPv4
peers of a dual-stack socket arrive through the kernel's IPv4 path, which reports
`IP_TOS` and never `IPV6_TCLASS`.

The two families use different widths, and the difference is not documented
anywhere obvious: `IP_TOS` arrives as a **single byte** and `IPV6_TCLASS` as a
**four-byte int**. `parse` branches on the length the kernel actually wrote
rather than on the family, so neither overreads.

## Writing a feature

Four static members and the concepts pick it up:

```cpp
struct my_feature {
    using value_type = ...;
    static constexpr std::size_t space{dgram::detail::space_for(sizeof(payload))};
    static bool matches(int level, int type) noexcept;
    static std::optional<value_type> parse(const ::cmsghdr*) noexcept;
    static std::size_t build(::cmsghdr* dst, const value_type&, dgram::family wire) noexcept;  // optional
    static dgram::result<> enable(int fd, dgram::family) noexcept;                        // optional
};
```

`cmsg_feature` requires only `value_type` and `space`, because not every feature
travels both ways: `parseable_feature` adds `matches` and `parse`,
`sendable_feature` adds `build`, and `receivable_feature` is a `parseable_feature`
with `enable`. `gro` is receivable but not sendable and `segment` the reverse, so
a receive and a transmit batch take their own sets.

A feature appearing in both families sets `space` to the larger payload.

`build` gets the family the datagram travels as, which is `inet4` for a
v4-mapped destination on a v6 socket. Emit the IPv4-level message for it.

`parse` returns nothing when the payload is narrower than what it would read.
That check is the feature's job, because only it knows what widths the kernel
uses for its own control messages.
