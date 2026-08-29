/**
 * @file timer.cppm
 * @brief Hierarchical timing wheel for protocol timeouts.
 *
 * Retransmission, loss detection and idle timeouts all want the same thing:
 * schedule and cancel constantly, expire rarely, and never pay a logarithm for
 * either. A sorted structure has the cost the wrong way round, since a protocol
 * cancels far more timers than it lets fire.
 *
 * Everything is indices into one carved block: no pointers, no allocation after
 * `carve`, and no destructor ever run. Not thread-safe, like the rest of the
 * engine.
 */
module;

#include <cassert>
#include <cerrno>

export module dgram:timer;

import std;
import libmem;

import :error;
import :pacing;

namespace dgram {

/**
 * @brief A scheduled timer.
 *
 * Carries a generation alongside the slot index, so a handle to a timer that
 * has since fired or been cancelled does not cancel whatever took its place.
 */
export class timer_handle {
public:
    constexpr timer_handle() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept { return generation_ != 0; }
    [[nodiscard]] friend constexpr bool operator==(const timer_handle&, const timer_handle&) noexcept = default;

private:
    template <typename, std::size_t, std::size_t, std::size_t> friend class timer_wheel;

    constexpr timer_handle(const std::uint32_t index, const std::uint32_t generation) noexcept : index_{index}, generation_{generation} {}

    std::uint32_t index_{};
    std::uint32_t generation_{}; ///< zero means "never scheduled"
};

/**
 * @brief Hierarchical timing wheel over a fixed number of timers.
 *
 * Time is counted in ticks of a caller-chosen duration. Each of `Levels` wheels
 * has `SlotsPerLevel` slots and covers `SlotsPerLevel` times the range of the
 * one below, so the whole structure spans `SlotsPerLevel ^ Levels` ticks while
 * scheduling and cancelling stay constant-time.
 *
 * Defaults span 256^4 ticks, which is about 49 days at a millisecond tick.
 *
 * @tparam Payload What to hand back when a timer fires: a flow key, or an index
 *                 into whatever holds protocol state.
 */
export template <typename Payload, std::size_t Capacity, std::size_t SlotsPerLevel = 256, std::size_t Levels = 4> class timer_wheel {
    static_assert(Capacity > 0);
    static_assert(SlotsPerLevel >= 2 && (SlotsPerLevel & (SlotsPerLevel - 1)) == 0, "SlotsPerLevel must be a power of two");
    static_assert(Levels >= 1 && Levels <= 8);
    static_assert(std::is_trivially_copyable_v<Payload>, "a timer payload is copied into the wheel and never destroyed");
    static_assert(Capacity < std::numeric_limits<std::uint32_t>::max() - 1, "indices are 32-bit");

public:
    using payload_type = Payload;

    static constexpr std::uint32_t nil{std::numeric_limits<std::uint32_t>::max()};
    static constexpr std::size_t slot_bits{std::countr_zero(SlotsPerLevel)};
    static constexpr std::size_t slot_mask{SlotsPerLevel - 1};
    static constexpr std::size_t slot_count{Levels * SlotsPerLevel};

    /** @brief Ticks the wheel can look ahead. Scheduling further out is refused. */
    static constexpr std::uint64_t horizon{std::uint64_t{1} << (Levels * slot_bits)};

    static constexpr std::size_t footprint() noexcept {
        return Capacity * sizeof(entry) + alignof(entry) - 1 + slot_count * sizeof(std::uint32_t) + alignof(std::uint32_t) - 1;
    }

    /**
     * @brief Carve the wheel and anchor it at `start`.
     *
     * @param tick Resolution. A timer fires on the first tick at or after its
     *             deadline, so this is the worst-case lateness.
     */
    template <libmem::aligned_monotonic_resource R>
    [[nodiscard]] static result<timer_wheel> carve(R& resource, const std::chrono::nanoseconds tick, const departure start) noexcept {
        if (tick <= std::chrono::nanoseconds::zero()) {
            return std::unexpected{invalid_argument};
        }
        timer_wheel w{};
        auto* entries{static_cast<entry*>(resource.allocate(Capacity * sizeof(entry), alignof(entry)))};
        auto* heads{static_cast<std::uint32_t*>(resource.allocate(slot_count * sizeof(std::uint32_t), alignof(std::uint32_t)))};
        if (entries == nullptr || heads == nullptr) [[unlikely]] {
            return std::unexpected{out_of_memory};
        }

        w.entries_ = {entries, Capacity};
        w.heads_ = {heads, slot_count};
        std::ranges::uninitialized_value_construct(w.entries_);
        std::ranges::fill(w.heads_, nil);

        w.tick_ = tick;
        w.epoch_ = start;
        w.now_tick_ = 0;

        // Free list through `next`, so allocation is a pop.
        for (std::uint32_t i{}; i < Capacity; ++i) {
            w.entries_[i].next = (i + 1 < Capacity) ? (i + 1) : nil;
            w.entries_[i].generation = 1; // zero is reserved for an invalid handle
        }
        w.free_ = 0;
        return w;
    }

    /**
     * @brief Schedule `payload` to fire at `when`.
     *
     * A deadline already past fires on the next `advance`.
     *
     * @return An invalid handle when the wheel is full, or when `when` is beyond
     *         `horizon` ticks away. Refusing is deliberate: silently clamping
     *         would fire the timer early, which is worse than not taking it.
     */
    [[nodiscard]] timer_handle schedule(const Payload& payload, const departure when) noexcept {
        // A deadline already past belongs in the next tick's slot, not in the
        // slot its stale tick maps to: that slot is a whole revolution away, so
        // the timer would fire far later than "immediately".
        const auto requested{tick_of(when)};
        const auto deadline{requested > now_tick_ ? requested : now_tick_ + 1};
        const auto delta{deadline - now_tick_};
        if (delta >= horizon) [[unlikely]] {
            return {};
        }
        if (free_ == nil) [[unlikely]] {
            return {};
        }

        const auto index{free_};
        free_ = entries_[index].next;

        auto& e{entries_[index]};
        e.payload = payload;
        e.deadline = deadline;
        e.live = true;
        link(index, slot_for(deadline, delta));
        ++size_;
        return timer_handle{index, e.generation};
    }

    /**
     * @brief Cancel a scheduled timer.
     * @return `false` if it already fired, was already cancelled, or the handle
     *         is stale, none of which are errors.
     */
    [[nodiscard]] bool cancel(const timer_handle handle) noexcept {
        if (!handle.valid() || handle.index_ >= Capacity) {
            return false;
        }
        auto& e{entries_[handle.index_]};
        if (!e.live || e.generation != handle.generation_) {
            return false;
        }
        unlink(handle.index_);
        release(handle.index_);
        return true;
    }

    /**
     * @brief Advance to `now`, firing everything due, oldest tick first.
     *
     * @param fire Called with each expired payload. Scheduling from inside it is
     *             safe; a timer scheduled for a tick already passed fires on the
     *             next `advance`, not recursively during this one.
     * @return How many timers fired.
     */
    template <typename Fire> std::size_t advance(const departure now, Fire&& fire) {
        const auto target{tick_of(now)};
        std::size_t fired{};

        while (now_tick_ < target) {
            ++now_tick_;

            // Cascade before firing: when the low wheel wraps, the timers for
            // this tick are still sitting in a coarser wheel.
            const auto index{now_tick_ & slot_mask};
            if (index == 0) {
                for (std::size_t level{1}; level < Levels; ++level) {
                    cascade(level);
                    if (((now_tick_ >> (level * slot_bits)) & slot_mask) != 0) {
                        break;
                    }
                }
            }

            fired += drain(head_at(0, index), std::forward<Fire>(fire));
        }
        return fired;
    }

    /** @brief Advance without firing, discarding whatever was due. */
    std::size_t advance(const departure now) {
        return advance(now, [](const Payload&) noexcept {});
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return free_ == nil; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    /** @brief The instant the wheel has advanced to. */
    [[nodiscard]] constexpr departure current() const noexcept { return epoch_ + tick_ * static_cast<std::int64_t>(now_tick_); }

    /** @brief Longest delay the wheel accepts. */
    [[nodiscard]] constexpr std::chrono::nanoseconds max_delay() const noexcept { return tick_ * static_cast<std::int64_t>(horizon - 1); }

private:
    struct entry {
        Payload payload{};
        std::uint64_t deadline{};
        std::uint32_t next{nil};
        std::uint32_t prev{nil};
        std::uint32_t slot{nil}; ///< which slot head owns this entry, so unlink is O(1)
        std::uint32_t generation{1};
        bool live{false};
    };

    timer_wheel() noexcept = default;

    [[nodiscard]] std::uint32_t& head_at(const std::size_t level, const std::size_t index) noexcept { return heads_[level * SlotsPerLevel + index]; }

    /** @brief Ticks since the epoch, rounded up so a timer never fires early. */
    [[nodiscard]] std::uint64_t tick_of(const departure when) const noexcept {
        const auto delta{when - epoch_};
        if (delta <= std::chrono::nanoseconds::zero()) {
            return 0;
        }
        const auto ns{delta.count()};
        const auto per{tick_.count()};
        return static_cast<std::uint64_t>((ns + per - 1) / per);
    }

    /** @brief Which level and slot a deadline `delta` ticks away belongs in. */
    [[nodiscard]] static std::size_t slot_for(const std::uint64_t deadline, const std::uint64_t delta) noexcept {
        for (std::size_t level{}; level < Levels; ++level) {
            const auto level_range{std::uint64_t{1} << ((level + 1) * slot_bits)};
            if (delta < level_range) {
                const auto index{(deadline >> (level * slot_bits)) & slot_mask};
                return level * SlotsPerLevel + static_cast<std::size_t>(index);
            }
        }
        return (Levels - 1) * SlotsPerLevel + static_cast<std::size_t>((deadline >> ((Levels - 1) * slot_bits)) & slot_mask);
    }

    void link(const std::uint32_t index, const std::size_t slot) noexcept {
        auto& head{heads_[slot]};
        entries_[index].prev = nil;
        entries_[index].next = head;
        if (head != nil) {
            entries_[head].prev = index;
        }
        head = index;
        entries_[index].slot = static_cast<std::uint32_t>(slot);
    }

    void unlink(const std::uint32_t index) noexcept {
        auto& e{entries_[index]};
        if (e.prev != nil) {
            entries_[e.prev].next = e.next;
        } else {
            heads_[e.slot] = e.next;
        }
        if (e.next != nil) {
            entries_[e.next].prev = e.prev;
        }
        e.next = nil;
        e.prev = nil;
    }

    /** @brief Return an entry to the free list, invalidating outstanding handles. */
    void release(const std::uint32_t index) noexcept {
        auto& e{entries_[index]};
        e.live = false;
        ++e.generation;
        if (e.generation == 0) {
            e.generation = 1; // never hand out the invalid generation
        }
        e.next = free_;
        free_ = index;
        --size_;
    }

    /** @brief Move a coarse slot's timers down to where they now belong. */
    void cascade(const std::size_t level) noexcept {
        const auto index{static_cast<std::size_t>((now_tick_ >> (level * slot_bits)) & slot_mask)};
        auto current{head_at(level, index)};
        head_at(level, index) = nil;

        while (current != nil) {
            const auto next{entries_[current].next};
            auto& e{entries_[current]};
            e.next = nil;
            e.prev = nil;
            const auto delta{e.deadline > now_tick_ ? e.deadline - now_tick_ : 0};
            link(current, slot_for(e.deadline, delta));
            current = next;
        }
    }

    /** @brief Fire and free everything in one slot. */
    template <typename Fire> std::size_t drain(std::uint32_t& head, Fire&& fire) {
        // Detach first: the callback may schedule, which must not touch this chain.
        auto current{head};
        head = nil;
        std::size_t fired{};

        while (current != nil) {
            const auto next{entries_[current].next};
            auto& e{entries_[current]};
            e.next = nil;
            e.prev = nil;
            const auto payload{e.payload};
            release(current);
            std::invoke(fire, payload);
            ++fired;
            current = next;
        }
        return fired;
    }

    std::span<entry> entries_{};
    std::span<std::uint32_t> heads_{};
    std::chrono::nanoseconds tick_{};
    departure epoch_{};
    std::uint64_t now_tick_{};
    std::uint32_t free_{nil};
    std::size_t size_{};
};

} // namespace dgram
