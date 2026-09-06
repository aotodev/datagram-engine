# Pacing

Writing datagrams as fast as the socket accepts them emits micro-bursts, which
overflow router queues and are read as loss by the congestion controller that
caused them. Pacing hands the kernel a departure time per datagram instead.

## The precondition, which fails silently

**`SO_TXTIME` does nothing unless the outgoing interface carries the Fair Queue
discipline.**

```sh
tc qdisc add dev <iface> root fq
```

Without it the socket option is accepted, every `SCM_TXTIME` control message is
accepted, every datagram is sent immediately, and **nothing is reported
anywhere**: not an error return, not the error queue. Measured, not assumed.

The engine does not detect this. Answering "is pacing actually active" needs a
netlink qdisc query, which is a hundred lines of `rtattr` parsing for a check
that fires once at startup, so it is deliberately out of scope. Treat `fq` as
part of deploying this, the way an MTU is.

## Setting up the socket

```cpp
auto sock{dgram::socket::open<dgram::transmit_time<>>(dgram::family::inet4)};
```

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `Clock` | `txtime_clock::monotonic` | Which clock departure times are on. |
| `Deadline` | `false` | `true` means "no later than" rather than "at", letting the qdisc reorder. |
| `ReportErrors` | `true` | Route missed and malformed departures to the error queue, where `etf` puts them and `fq` does not. |

**`CLOCK_TAI` requires `CAP_NET_ADMIN`** and fails with `EPERM` otherwise, which
is why `monotonic` is the default. Reach for `tai` when departure times are
shared between machines, since it does not step.

`dgram::pacing_clock` is `std::chrono::steady_clock`, which is `CLOCK_MONOTONIC`
on this platform. `now_on(clock)` reads whichever clock the socket was configured
with.

**The socket option is not conditional.** `transmit_time<>` is chosen when the
socket is opened, so a node that may or may not pace either decides at open time
or carries `SO_TXTIME` always and omits the `txtime` control message per
datagram. The second works and is the usual answer; there is no way to add the
option later.

## Attaching a departure time

`txtime` is a send-only feature, per datagram, so a batch can mix paced and
unpaced entries:

```cpp
using tx_features = dgram::features<dgram::txtime, dgram::ecn>;

dgram::control<tx_features> ancillary{};
ancillary.set<dgram::txtime>(when);
(void)tx.stage(payload, peer, ancillary);
```

The control message type and the socket option share a value
(`SCM_TXTIME == SO_TXTIME == 61`). That is the kernel's own aliasing.

**With `segment`, one departure covers every segment in the buffer.** The
kernel is handed one buffer, one control message and slices it, so there is
nowhere to put a per-segment departure time. Pacing granularity becomes the
buffer, and a coalesced buffer leaves as a burst: measured at a nominal 10 ms
per datagram under `fq`, one datagram per buffer spreads them 9.2 ms to 10.7 ms
apart, while seven segments per buffer put 1.2 us between segments and 70.9 ms
between buffers. The total spread is the same, which is why a test measuring
first arrival against last cannot tell the two apart.

That is not a defect in either feature, but it is the opposite of the reason
for pacing given at the top of this page: a sender that wants per-datagram
pacing must not coalesce. See [segmentation offload](offload.md).

## Computing the departure: `pacer`

```cpp
dgram::pacer p{bytes_per_second, dgram::now_on(dgram::txtime_clock::monotonic)};

ancillary.set<dgram::txtime>(p.schedule(wire_size));
```

| Member | Description |
|--------|-------------|
| `pacer(rate, start)` | Bytes per second, anchored at a departure time. Rate zero is unpaced. |
| `departure schedule(bytes)` | The departure for this datagram, advancing the schedule. |
| `void resume_at(now)` | Re-anchor forward after an idle period. Never rewinds. |
| `void set_rate(rate)` | Change rate without moving the current departure. |
| `departure peek()` | The next departure, without consuming it. |

`schedule` charges `bytes` against the rate. Pass the wire size, not just the
payload, or the pacer runs slightly fast.

**Call `resume_at` after an idle period.** A sender that stops for a second comes
back with a schedule a second in the past and emits its whole backlog as fast as
the socket accepts, which is the burst pacing exists to prevent. `resume_at`
only ever moves the schedule forward.

`pacer` is drift-free: the sub-nanosecond remainder of each division is carried
into the next datagram rather than truncated. Accumulating rounded intervals is
the usual way this goes wrong, and it is not a rounding curiosity: at 1500-byte
datagrams and a rate that does not divide evenly, truncation loses about 70
microseconds per hundred thousand datagrams and grows without bound.

It is `constexpr`, has no state beyond three integers, and is not thread-safe:
one pacer per sending thread, like everything else here.

## Errors

With `ReportErrors` on, the kernel reports a departure it could not honour:

```cpp
if (const auto fault{dgram::drain_transmit_error(sock.native())}; fault && *fault) {
    switch (**fault) { /* invalid_departure, missed_deadline, other */ }
}
```

An empty queue is a success with no value, not an error.

**`fq` never reports anything here, and neither does no discipline at all.**
The two queueing disciplines that honour `SO_TXTIME` do different things with a
departure they cannot meet: `fq` holds what it can and silently drops the rest,
including anything past its `horizon` (10 s by default, with `horizon_drop` on),
while `etf` answers on the error queue. Nothing in `fq` calls
`SO_EE_ORIGIN_TXTIME`, so a program that follows the pacing advice above and
then waits for a fault waits forever.

`etf` is stricter in exchange: it insists its own clock matches the socket's, and
it wants `CLOCK_TAI`, which needs `CAP_NET_ADMIN`. A datagram from a socket on
another clock is refused before its departure time is even looked at, and the
refusal is what arrives on the error queue.

| Discipline | Honours a departure | Reports one it refuses |
|------------|---------------------|------------------------|
| none | no, silently | no |
| `fq` | yes | **no** |
| `etf` | yes | yes, as `invalid_departure` or `missed_deadline` |

So `ReportErrors` is worth setting either way, since it costs nothing when
nothing reports, but treat a quiet error queue under `fq` as no information
rather than as confirmation that pacing is working.

## Pacing is not a timer

The two are often discussed together and are different things. Pacing hands the
kernel a departure time for a datagram the engine is already holding; a timer
schedules a callback against connection state. See [timers](timers.md) for the
latter.
