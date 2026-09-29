// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file timer_tests.cpp
 * @brief The hierarchical timing wheel.
 *
 * The cascade is the part that is subtly wrong rather than obviously wrong: a
 * timer landing one slot off, or a coarse wheel not being pulled down before the
 * tick it covers, shows up only at particular delays and particular start
 * offsets. Most of what is below is therefore differential against a naive
 * reference that cannot get ordering wrong.
 */
#include <gtest/gtest.h>

import std;
import libmem;
import dgram;

namespace {

using namespace std::chrono_literals;

constexpr auto tick{1ms};
constexpr std::size_t capacity{512};

/* Small wheel on purpose: 4 slots per level makes the cascade happen every four
   ticks instead of every 256, so the tests actually reach it. */
using wheel = dgram::timer_wheel<std::uint32_t, capacity, 4, 4>;

/** What the wheel should do, computed the slow obvious way. */
class reference {
public:
    void schedule(const std::uint32_t id, const std::uint64_t deadline_tick) { pending_.emplace_back(deadline_tick, id); }

    void cancel(const std::uint32_t id) {
        std::erase_if(pending_, [id](const auto& p) { return p.second == id; });
    }

    /** Everything due at or before `tick`, in tick order. */
    std::vector<std::uint32_t> advance_to(const std::uint64_t to) {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> due{};
        auto rest{pending_};
        pending_.clear();
        for (const auto& p : rest) {
            (p.first <= to ? due : pending_).push_back(p);
        }
        std::ranges::stable_sort(due, {}, &std::pair<std::uint64_t, std::uint32_t>::first);
        std::vector<std::uint32_t> ids{};
        for (const auto& p : due) {
            ids.push_back(p.second);
        }
        return ids;
    }

    [[nodiscard]] std::size_t size() const noexcept { return pending_.size(); }

private:
    std::vector<std::pair<std::uint64_t, std::uint32_t>> pending_{};
};

struct fixture {
    libmem::arena arena{wheel::footprint()};
    wheel w{*wheel::carve(arena, tick, 0ns)};
};

std::vector<std::uint32_t> fire_all(wheel& w, const dgram::departure to) {
    std::vector<std::uint32_t> fired{};
    const auto count{w.advance(to, [&](const std::uint32_t id) { fired.push_back(id); })};
    EXPECT_EQ(count, fired.size());
    return fired;
}

/* ============================================================================
 * Basics
 * ============================================================================ */

TEST(TimerWheel, FiresAtTheDeadlineAndNotBefore) {
    fixture f{};
    ASSERT_TRUE(f.w.schedule(7, 5ms).valid());

    EXPECT_TRUE(fire_all(f.w, 4ms).empty()) << "not due yet";
    EXPECT_EQ(f.w.size(), 1u);
    EXPECT_EQ(fire_all(f.w, 5ms), (std::vector<std::uint32_t>{7}));
    EXPECT_EQ(f.w.size(), 0u);
    EXPECT_TRUE(fire_all(f.w, 100ms).empty()) << "a fired timer does not fire twice";
}

TEST(TimerWheel, DeadlineInThePastFiresOnTheNextAdvance) {
    fixture f{};
    ASSERT_TRUE(f.w.advance(50ms) == 0);
    ASSERT_TRUE(f.w.schedule(1, 10ms).valid()) << "already past";
    EXPECT_EQ(fire_all(f.w, 51ms), (std::vector<std::uint32_t>{1}));
}

TEST(TimerWheel, RoundsUpSoATimerIsNeverEarly) {
    fixture f{};
    // 1500us is a tick and a half; it must fire at tick 2, not tick 1.
    ASSERT_TRUE(f.w.schedule(1, 1500us).valid());
    EXPECT_TRUE(fire_all(f.w, 1ms).empty()) << "firing at tick 1 would be early";
    EXPECT_EQ(fire_all(f.w, 2ms), (std::vector<std::uint32_t>{1}));
}

TEST(TimerWheel, CancelStopsATimer) {
    fixture f{};
    const auto h{f.w.schedule(3, 5ms)};
    ASSERT_TRUE(h.valid());
    EXPECT_TRUE(f.w.cancel(h));
    EXPECT_EQ(f.w.size(), 0u);
    EXPECT_TRUE(fire_all(f.w, 100ms).empty());
    EXPECT_FALSE(f.w.cancel(h)) << "cancelling twice is false, not an error";
}

/* A handle to a timer that already fired must not cancel whatever reused its
   slot. This is the ABA the generation counter exists for. */
TEST(TimerWheel, StaleHandleDoesNotCancelItsSuccessor) {
    fixture f{};
    const auto first{f.w.schedule(1, 5ms)};
    ASSERT_EQ(fire_all(f.w, 5ms), (std::vector<std::uint32_t>{1}));

    const auto second{f.w.schedule(2, 10ms)};
    ASSERT_TRUE(second.valid());
    EXPECT_FALSE(f.w.cancel(first)) << "the stale handle must be rejected";
    EXPECT_EQ(f.w.size(), 1u) << "the new timer must survive";
    EXPECT_EQ(fire_all(f.w, 10ms), (std::vector<std::uint32_t>{2}));
}

TEST(TimerWheel, DefaultHandleIsInvalidAndHarmless) {
    fixture f{};
    EXPECT_FALSE(dgram::timer_handle{}.valid());
    EXPECT_FALSE(f.w.cancel(dgram::timer_handle{}));
}

TEST(TimerWheel, RefusesWhenFull) {
    fixture f{};
    for (std::size_t i{}; i < capacity; ++i) {
        ASSERT_TRUE(f.w.schedule(static_cast<std::uint32_t>(i), 5ms).valid()) << "at " << i;
    }
    EXPECT_TRUE(f.w.full());
    EXPECT_FALSE(f.w.schedule(9999, 5ms).valid()) << "a full wheel refuses rather than overwriting";
    EXPECT_EQ(f.w.size(), capacity);
}

/* Clamping a too-distant deadline would fire the timer early, which is worse
   than declining to take it. */
TEST(TimerWheel, RefusesBeyondTheHorizon) {
    fixture f{};
    EXPECT_EQ(wheel::horizon, 256u) << "4 slots, 4 levels";
    EXPECT_TRUE(f.w.schedule(1, tick * (wheel::horizon - 1)).valid());
    EXPECT_FALSE(f.w.schedule(2, tick * wheel::horizon).valid());
    EXPECT_FALSE(f.w.schedule(3, 24h).valid());
    EXPECT_EQ(f.w.max_delay(), tick * (wheel::horizon - 1));
}

TEST(TimerWheel, CarveRejectsANonPositiveTick) {
    libmem::arena arena{wheel::footprint()};
    EXPECT_FALSE(wheel::carve(arena, 0ns, 0ns).has_value());
    EXPECT_FALSE(wheel::carve(arena, -1ms, 0ns).has_value());
}

TEST(TimerWheel, CarveFailsCleanlyOnASmallArena) {
    libmem::arena arena{16};
    EXPECT_FALSE(wheel::carve(arena, tick, 0ns).has_value());
}

/* ============================================================================
 * Cascade
 * ============================================================================ */

/* With 4 slots per level, anything past tick 4 starts in a coarser wheel and
   has to be pulled down before it can fire. Every delay here crosses at least
   one level boundary. */
TEST(TimerWheel, FiresAcrossEveryLevelBoundary) {
    for (std::uint64_t delay{1}; delay < wheel::horizon; ++delay) {
        fixture f{};
        ASSERT_TRUE(f.w.schedule(42, tick * static_cast<std::int64_t>(delay)).valid()) << "delay " << delay;

        // One tick early: nothing.
        if (delay > 1) {
            ASSERT_TRUE(fire_all(f.w, tick * static_cast<std::int64_t>(delay - 1)).empty()) << "early at delay " << delay;
        }
        EXPECT_EQ(fire_all(f.w, tick * static_cast<std::int64_t>(delay)), (std::vector<std::uint32_t>{42})) << "delay " << delay;
    }
}

/* Same, but advancing one tick at a time, which exercises the cascade on every
   wrap rather than jumping over it. */
TEST(TimerWheel, FiresOnTheRightTickWhenSteppedOneTickAtATime) {
    for (std::uint64_t delay{1}; delay < wheel::horizon; ++delay) {
        fixture f{};
        ASSERT_TRUE(f.w.schedule(1, tick * static_cast<std::int64_t>(delay)).valid());

        std::uint64_t fired_at{};
        for (std::uint64_t t{1}; t <= delay + 2 && fired_at == 0; ++t) {
            if (!fire_all(f.w, tick * static_cast<std::int64_t>(t)).empty()) {
                fired_at = t;
            }
        }
        EXPECT_EQ(fired_at, delay) << "delay " << delay;
    }
}

/* ============================================================================
 * Differential
 * ============================================================================ */

TEST(TimerWheel, MatchesANaiveReferenceUnderChurn) {
    fixture f{};
    reference ref{};
    std::mt19937 rng{20240829};

    std::vector<std::pair<dgram::timer_handle, std::uint32_t>> live{};
    std::uint32_t next_id{1};
    std::uint64_t now{};

    for (int round{}; round < 4000; ++round) {
        const auto action{rng() % 10};

        if (action < 5 && live.size() < capacity - 1) {
            const auto delay{1 + (rng() % (wheel::horizon - 1))};
            const auto id{next_id++};
            const auto h{f.w.schedule(id, tick * static_cast<std::int64_t>(now + delay))};
            ASSERT_TRUE(h.valid()) << "round " << round;
            ref.schedule(id, now + delay);
            live.emplace_back(h, id);
        } else if (action < 7 && !live.empty()) {
            const auto pick{rng() % live.size()};
            const auto [h, id]{live[pick]};
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(pick));
            if (f.w.cancel(h)) {
                ref.cancel(id);
            }
        } else {
            const auto step{1 + (rng() % 6)};
            now += step;
            auto fired{fire_all(f.w, tick * static_cast<std::int64_t>(now))};
            auto expected{ref.advance_to(now)};

            // Order within one tick is unspecified; order between ticks is not,
            // and the reference is sorted by tick, so compare as multisets.
            std::ranges::sort(fired);
            std::ranges::sort(expected);
            ASSERT_EQ(fired, expected) << "round " << round << " at tick " << now;

            std::erase_if(live, [&](const auto& p) { return std::ranges::find(expected, p.second) != expected.end(); });
        }
        ASSERT_EQ(f.w.size(), ref.size()) << "round " << round;
    }
}

/* ============================================================================
 * Interaction with the loop
 * ============================================================================ */

/* A protocol reschedules from inside the callback constantly: a retransmission
   timer that fires arms the next one. That must not fire again in the same
   advance, and must not corrupt the slot being drained. */
TEST(TimerWheel, ReschedulingFromTheCallbackIsSafe) {
    fixture f{};
    int fires{};
    std::function<void(std::uint32_t)> rearm = [&](std::uint32_t id) {
        ++fires;
        if (fires < 5) {
            EXPECT_TRUE(f.w.schedule(id, f.w.current() + 3ms).valid());
        }
    };

    ASSERT_TRUE(f.w.schedule(1, 3ms).valid());
    for (std::int64_t t{1}; t <= 30; ++t) {
        (void)f.w.advance(tick * t, rearm);
    }
    EXPECT_EQ(fires, 5) << "each firing arms exactly one successor";
    EXPECT_EQ(f.w.size(), 0u);
}

/* Timers of one connection often share a tick, and the first to fire cancels the
   rest. Those are still waiting in the slot being drained. */
TEST(TimerWheel, CancellingATimerDueOnTheSameTickFromTheCallbackIsSafe) {
    fixture f{};
    const std::array handles{f.w.schedule(1, 5ms), f.w.schedule(2, 5ms), f.w.schedule(3, 5ms)};
    ASSERT_TRUE(std::ranges::all_of(handles, &dgram::timer_handle::valid));

    std::size_t cancelled{};
    const auto fired{f.w.advance(5ms, [&](const std::uint32_t id) {
        for (const auto [other, handle] : std::views::zip(std::views::iota(1U), handles)) {
            if (other != id && f.w.cancel(handle)) {
                ++cancelled;
            }
        }
    })};
    EXPECT_EQ(fired, 1u) << "the first to fire cancelled the other two";
    EXPECT_EQ(cancelled, 2u);
    EXPECT_EQ(f.w.size(), 0u);
}

/* Rearming reuses the entry that just fired, so a stale link to it from the slot
   being drained would reach whichever chain the new timer joined. */
TEST(TimerWheel, RearmingThenCancellingASiblingKeepsOtherSlotsIntact) {
    fixture f{};
    const std::array handles{f.w.schedule(1, 5ms), f.w.schedule(2, 5ms), f.w.schedule(3, 5ms)};
    ASSERT_TRUE(f.w.schedule(4, 8ms).valid());

    std::vector<std::uint32_t> fired{};
    bool rearmed{};
    (void)f.w.advance(5ms, [&](const std::uint32_t id) {
        fired.push_back(id);
        if (!std::exchange(rearmed, true)) {
            EXPECT_TRUE(f.w.schedule(30, 8ms).valid());
            for (const auto handle : handles) {
                (void)f.w.cancel(handle);
            }
        }
    });
    auto later{fire_all(f.w, 20ms)};
    std::ranges::sort(later);
    EXPECT_EQ(fired.size(), 1u);
    EXPECT_EQ(later, (std::vector<std::uint32_t>{4, 30}));
    EXPECT_EQ(f.w.size(), 0u);
}

TEST(TimerWheel, ManyTimersOnOneTickAllFire) {
    fixture f{};
    for (std::uint32_t i{}; i < 100; ++i) {
        ASSERT_TRUE(f.w.schedule(i, 7ms).valid());
    }
    auto fired{fire_all(f.w, 7ms)};
    std::ranges::sort(fired);
    ASSERT_EQ(fired.size(), 100u);
    EXPECT_EQ(fired.front(), 0u);
    EXPECT_EQ(fired.back(), 99u);
    EXPECT_EQ(f.w.size(), 0u);
}

TEST(TimerWheel, CurrentTracksTheAdvancedInstant) {
    fixture f{};
    EXPECT_EQ(f.w.current(), 0ns);
    (void)f.w.advance(10ms);
    EXPECT_EQ(f.w.current(), 10ms);
    (void)f.w.advance(5ms);
    EXPECT_EQ(f.w.current(), 10ms) << "advance never goes backwards";
}

/* A wheel anchored at a real clock reading must behave the same as one at zero:
   the level a timer lands in depends on the absolute tick, not just the delay. */
TEST(TimerWheel, WorksFromAnArbitraryEpoch) {
    for (const auto offset : {0ns, 1ns, 999'999ns, 12'345'678ns}) {
        libmem::arena arena{wheel::footprint()};
        auto w{wheel::carve(arena, tick, offset)};
        ASSERT_TRUE(w.has_value());

        for (std::uint64_t delay{1}; delay < wheel::horizon; ++delay) {
            auto scratch_arena{libmem::arena{wheel::footprint()}};
            auto fresh{wheel::carve(scratch_arena, tick, offset)};
            ASSERT_TRUE(fresh.has_value());
            ASSERT_TRUE(fresh->schedule(1, offset + tick * static_cast<std::int64_t>(delay)).valid());
            EXPECT_EQ(fire_all(*fresh, offset + tick * static_cast<std::int64_t>(delay)).size(), 1u) << "offset " << offset.count() << " delay " << delay;
        }
    }
}

/* ============================================================================
 * Waiting on the wheel
 *
 * What a loop that blocks on a socket needs: how long until something is due.
 * Every case below is one a poll loop actually hits.
 * ============================================================================ */

TEST(TimerWheel, NextDeadlineIsEmptyWhenNothingIsArmed) {
    libmem::arena arena{wheel::footprint()};
    auto w{wheel::carve(arena, tick, 0ns)};
    ASSERT_TRUE(w.has_value());
    EXPECT_FALSE(w->next_deadline().has_value());

    const auto handle{w->schedule(1, 5ms)};
    ASSERT_TRUE(handle.valid());
    EXPECT_TRUE(w->cancel(handle));
    EXPECT_FALSE(w->next_deadline().has_value()) << "cancelling the last timer empties the wheel again";
}

TEST(TimerWheel, NextDeadlineIsTheSoonestArmed) {
    libmem::arena arena{wheel::footprint()};
    auto w{wheel::carve(arena, tick, 0ns)};
    ASSERT_TRUE(w.has_value());

    (void)w->schedule(1, 40ms);
    (void)w->schedule(2, 7ms);
    (void)w->schedule(3, 19ms);
    ASSERT_TRUE(w->next_deadline().has_value());
    EXPECT_EQ(*w->next_deadline(), 7ms) << "order of scheduling must not matter";
}

/* The wheels are searched level by level, so a deadline that has not cascaded
   down yet still has to be found, and found before a nearer one is missed. */
TEST(TimerWheel, NextDeadlineFindsTimersOnEveryLevel) {
    libmem::arena arena{wheel::footprint()};
    auto w{wheel::carve(arena, tick, 0ns)};
    ASSERT_TRUE(w.has_value());

    // 4 slots per level: level 0 spans 4 ticks, level 1 sixteen, level 2 sixty-four.
    (void)w->schedule(1, 200ms);
    ASSERT_TRUE(w->next_deadline().has_value());
    EXPECT_EQ(*w->next_deadline(), 200ms) << "a timer three levels up is still the soonest when it is the only one";

    (void)w->schedule(2, 30ms);
    EXPECT_EQ(*w->next_deadline(), 30ms);
    (void)w->schedule(3, 6ms);
    EXPECT_EQ(*w->next_deadline(), 6ms);
    (void)w->schedule(4, 2ms);
    EXPECT_EQ(*w->next_deadline(), 2ms);
}

/* The property that matters to a loop: waiting exactly that long and no longer
   always lands on a tick where something fires. */
TEST(TimerWheel, WaitingForNextDeadlineAlwaysWakesToWork) {
    libmem::arena arena{wheel::footprint()};
    auto w{wheel::carve(arena, tick, 0ns)};
    ASSERT_TRUE(w.has_value());

    std::mt19937 rng{20260830};
    std::uniform_int_distribution<std::uint64_t> delay{1, 300};
    std::size_t armed{};
    for (std::uint32_t id{}; id < 40; ++id) {
        // Some of these are past the horizon on a 4-by-4 wheel and are refused,
        // which is the wheel doing its job; only what it took has to fire.
        armed += static_cast<std::size_t>(w->schedule(id, tick * delay(rng)).valid());
    }
    ASSERT_GT(armed, 0u);
    ASSERT_LT(armed, 40u) << "this wheel's horizon should refuse some of these, or the case is untested";

    std::size_t fired{};
    std::size_t wakeups{};
    while (const auto deadline{w->next_deadline()}) {
        ++wakeups;
        const auto now{*deadline};
        const auto count{w->advance(now, [&](const std::uint32_t) noexcept { ++fired; })};
        EXPECT_GT(count, 0u) << "waking at next_deadline must find something due, not an empty tick";
        ASSERT_LT(wakeups, 100u) << "the wheel must drain, not stall";
    }
    EXPECT_EQ(fired, armed);
}

/* The slot the cursor sits on is the one whose index says nothing: everything
   else at a level maps to exactly one period, but this one also collects what
   wrapped onto it a whole revolution later. A wheel whose only timer is one of
   those has to find it anyway. */
TEST(TimerWheel, NextDeadlineFindsATimerWrappedOntoTheCursorSlot) {
    libmem::arena arena{wheel::footprint()};
    auto w{wheel::carve(arena, tick, 0ns)};
    ASSERT_TRUE(w.has_value());

    // Four slots per level: at tick 3, level 1 sits on slot 0, and a deadline 15
    // ticks out lands on slot 0 as well, a full revolution of that level away.
    EXPECT_EQ(w->advance(3ms), 0u);
    ASSERT_TRUE(w->schedule(1, 18ms).valid());

    ASSERT_TRUE(w->next_deadline().has_value());
    EXPECT_EQ(*w->next_deadline(), 18ms);
    EXPECT_EQ(w->advance(*w->next_deadline(), [](const std::uint32_t) noexcept {}), 1u) << "and waiting that long has to actually fire it";
}

TEST(TimerWheel, NextDeadlineMatchesTheNaiveMinimumUnderChurn) {
    libmem::arena arena{wheel::footprint()};
    auto w{wheel::carve(arena, tick, 0ns)};
    ASSERT_TRUE(w.has_value());

    std::mt19937 rng{7};
    std::uniform_int_distribution<std::uint64_t> delay{1, 500};
    std::map<std::uint32_t, std::uint64_t> armed{};
    std::vector<std::pair<std::uint32_t, dgram::timer_handle>> live{};

    std::uint64_t now_tick{};
    for (std::uint32_t round{}; round < 400; ++round) {
        if (live.size() < 30) {
            const auto at{now_tick + delay(rng)};
            const auto handle{w->schedule(round, tick * static_cast<std::int64_t>(at))};
            if (handle.valid()) {
                armed[round] = at;
                live.emplace_back(round, handle);
            }
        } else {
            const auto victim{live.back()};
            live.pop_back();
            if (w->cancel(victim.second)) {
                armed.erase(victim.first);
            }
        }

        now_tick += 3;
        const auto now{tick * static_cast<std::int64_t>(now_tick)};
        (void)w->advance(now, [&](const std::uint32_t id) noexcept { armed.erase(id); });
        std::erase_if(live, [&](const auto& p) { return !armed.contains(p.first); });

        const auto expected{std::ranges::min_element(armed, {}, [](const auto& p) { return p.second; })};
        if (expected == armed.end()) {
            EXPECT_FALSE(w->next_deadline().has_value()) << "round " << round;
        } else {
            ASSERT_TRUE(w->next_deadline().has_value()) << "round " << round;
            EXPECT_EQ(*w->next_deadline(), tick * static_cast<std::int64_t>(expected->second)) << "round " << round;
        }
    }
}

TEST(TimerWheel, TimeToNextIsNeverNegative) {
    libmem::arena arena{wheel::footprint()};
    auto w{wheel::carve(arena, tick, 0ns)};
    ASSERT_TRUE(w.has_value());

    (void)w->schedule(1, 5ms);
    ASSERT_TRUE(w->time_to_next(0ns).has_value());
    EXPECT_EQ(*w->time_to_next(0ns), 5ms);
    EXPECT_EQ(*w->time_to_next(4ms), 1ms);
    EXPECT_EQ(*w->time_to_next(500ms), 0ns) << "a deadline already past means wait no time, not a negative wait";
}

} // namespace
