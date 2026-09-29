# Timers

Retransmission, loss detection and idle timeouts all want the same shape:
schedule and cancel constantly, expire rarely. A sorted structure has the cost
the wrong way round, because a protocol cancels far more timers than it lets
fire. A hierarchical timing wheel makes both constant-time.

```cpp
using wheel = dgram::timer_wheel<flow_id, 4096>;

libmem::arena arena{wheel::footprint()};
auto timers{wheel::carve(arena, 1ms, dgram::now_on(dgram::txtime_clock::monotonic))};
```

Carved once from a resource, like everything else: no pointers, no allocation
after `carve`, and no destructor ever run.

## Shape

```cpp
template <typename Payload, std::size_t Capacity,
          std::size_t SlotsPerLevel = 256, std::size_t Levels = 4>
class timer_wheel;
```

Time is counted in ticks of a caller-chosen duration. Each of `Levels` wheels
has `SlotsPerLevel` slots and covers `SlotsPerLevel` times the range of the one
below, so the structure spans `SlotsPerLevel ^ Levels` ticks. The defaults span
`256^4` ticks, about **49 days at a millisecond tick**.

`Payload` is what comes back when a timer fires: a flow key, or an index into
whatever holds protocol state. It must be trivially copyable, since the wheel
copies it in and never destroys it.

The tick is the resolution and therefore the worst-case lateness. A timer fires
on the first tick **at or after** its deadline, never before.

## Using it

| Member | Description |
|--------|-------------|
| `timer_handle schedule(payload, when)` | Arm a timer. Invalid handle if full or beyond `max_delay()`. |
| `bool cancel(handle)` | `false` if it already fired or the handle is stale, neither an error. |
| `size_t advance(now, fire)` | Fire everything due, oldest tick first. Returns how many. |
| `optional<departure> next_deadline()` | When the soonest armed timer is due, or nothing. |
| `optional<nanoseconds> time_to_next(now)` | The same, as a wait, floored at zero. |
| `departure current()` | The instant the wheel has advanced to. |
| `nanoseconds max_delay()` | Longest delay accepted. |

```cpp
const auto now{dgram::now_on(dgram::txtime_clock::monotonic)};
(void)timers.advance(now, [&](const flow_id id) {
    if (auto* flow = flows.find(id)) { flow->on_timeout(now); }
});
```

Drive it once per loop iteration, next to `receive`.

## Waiting on it

A loop that blocks on a socket has to wake for whichever comes first, so it
needs to know when the wheel next wants attention:

```cpp
const auto limit{timers.time_to_next(now).value_or(budget)};
::ppoll(&fd, 1, &as_timespec(std::min(budget, limit)), nullptr);
```

Without this a caller has only two options, and both are worse: wake on a fixed
tick and burn a syscall whether or not anything is due, or keep a parallel copy
of every deadline alongside the wheel, which is the bookkeeping the wheel exists
to remove.

`next_deadline` searches every level, not just the finest. A timer moves down a
level only when a cascade reaches it, so between cascades it sits at the level
its delay warranted when it was scheduled rather than the level its *remaining*
delay warrants now, and a coarse wheel can therefore hold a nearer deadline than
a fine one. The cost is `Levels * SlotsPerLevel` slot probes and two chain walks
per level, independent of how many timers are armed.

**Rescheduling from inside the callback is safe and expected:** a
retransmission timer that fires arms the next one. A timer scheduled for a tick
already passed fires on the *next* `advance`, not recursively during this one, so
a rearming timer cannot spin the loop. Cancelling from the callback is safe too,
including a timer due on the same tick that has not fired yet.

## Handles are generation-checked

A `timer_handle` carries a generation alongside its slot index. When a timer
fires or is cancelled its slot returns to the free list with the generation
bumped, so a handle kept past that point is rejected rather than cancelling
whatever took the slot. Protocol code routinely holds a handle it is not sure is
still live; that has to be safe.

A default-constructed handle is invalid and cancelling it is a no-op, so there is
no need for a separate "armed" flag alongside one.

## What it refuses

**Full**: `schedule` returns an invalid handle rather than evicting something.

**Beyond the horizon**: also an invalid handle. Clamping a too-distant deadline
to the last slot would fire the timer *early*, which is worse than not accepting
it. Widen the tick or add a level if you need longer than `max_delay()`.

Both are the same refuse-rather-than-degrade choice as `flow_table::insert` and
`transmit_batch::stage`.

## Sizing

`SlotsPerLevel` and `Levels` trade memory against range: slot heads cost
`Levels * SlotsPerLevel * 4` bytes, so the defaults are 4 KB of heads regardless
of `Capacity`. `Capacity` bounds how many timers can be armed at once, which for
most protocols is a small multiple of the connection count.

A smaller `SlotsPerLevel` cascades more often but costs less memory; the tests
use 4 slots per level precisely to make the cascade happen constantly.
