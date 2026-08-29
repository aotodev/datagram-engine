# Ancillary data

Datagrams carry more than payload and peer address. The kernel reports which
local address one arrived on, and what its ECN marking was, as *control
messages* alongside the data. This layer turns those into typed values and
attaches them on the way out.

## The feature set

A feature is a type. The set of them is a compile-time list, and that one list
determines three things that must agree: how large the control buffer is, what
the parser looks for, and what can be attached on transmit.

```cpp
using metadata_set = dgram::features<dgram::pktinfo, dgram::ecn>;

using rx = dgram::receive_batch<64, 2048, metadata_set>;
using tx = dgram::transmit_batch<64, 0, metadata_set>;
```

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
dgram::control<metadata_set> reply{};
reply.set<dgram::ecn>(dgram::ecn_codepoint::ect0);
(void)tx.stage(payload, peer, reply);
```

Unset features contribute nothing. The control block is built per datagram and
`msg_controllen` set to exactly what was written, zero included, so a marking on
one datagram cannot leak onto the next one staged in the same slot.

The family for the build step comes from the destination endpoint, so a
dual-stack socket sending to both families emits the right level and type per
datagram.

## `pktinfo`

`IP_PKTINFO` / `IPV6_PKTINFO`. Value type is `local_info`:

| Field | Description |
|-------|-------------|
| `endpoint address` | The local address the datagram was sent to. Port is not populated. |
| `unsigned int interface` | Interface index, `0` when unspecified. |

The reason a wildcard-bound socket can answer from the address the peer actually
addressed, which matters the moment a host has more than one.

On transmit it selects the source address and interface. Leaving both zero lets
the routing table choose, which is the same as attaching nothing.

Sized for the larger of the two families' payloads, because the receive path
cannot know which will arrive: a dual-stack socket reports `IPV6_PKTINFO` even
for v4-mapped traffic.

## `ecn`

`IP_TOS` / `IPV6_TCLASS`, reduced to the two ECN bits of RFC 3168.

| `ecn_codepoint` | Meaning |
|-----------------|---------|
| `not_ect` | Not ECN-capable transport. |
| `ect1` | ECN-capable, codepoint 1. |
| `ect0` | ECN-capable, codepoint 0. |
| `ce` | Congestion experienced. |

An unmarked datagram reads as `not_ect`, not as absent, provided reporting was
enabled.

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
    static std::size_t build(::cmsghdr* dst, const value_type&, dgram::family) noexcept;  // optional
    static dgram::result<> enable(int fd, dgram::family) noexcept;                        // optional
};
```

`cmsg_feature` requires the first four; `sendable_feature` adds `build` and
`receivable_feature` adds `enable`. A feature appearing in both families sets
`space` to the larger payload.

`parse` returns nothing when the payload is narrower than what it would read.
That check is the feature's job, because only it knows what widths the kernel
uses for its own control messages.
