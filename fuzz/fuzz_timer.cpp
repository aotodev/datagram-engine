// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file fuzz_timer.cpp
 * @brief Differential fuzzer for the timing wheel.
 *
 * The wheel is not attacker-facing, but it is index arithmetic across four
 * levels with a cascade that only runs on wrap ticks, so it is the shape that is
 * right for most delays and wrong for a few. Driving it against a naive
 * reference reaches the combinations of start offset, delay and step size that
 * a fixed test does not.
 *
 * Checked at every step: the set of fired timers matches the reference exactly,
 * nothing fires early, nothing is lost, and the sizes agree.
 */
#include <cstddef>
#include <cstdint>

import std;
import libmem;
import dgram;

namespace {

using namespace std::chrono_literals;

/* Deliberately tiny: 4 slots per level makes the cascade fire constantly. */
constexpr std::size_t capacity{128};
using wheel = dgram::timer_wheel<std::uint32_t, capacity, 4, 4>;

constexpr auto tick{1ms};

/** A byte stream read as a sequence of small operations. */
class cursor {
public:
    explicit cursor(std::span<const std::uint8_t> data) noexcept : data_{data} {}

    [[nodiscard]] bool done() const noexcept { return at_ >= data_.size(); }
    [[nodiscard]] std::uint8_t next() noexcept { return at_ < data_.size() ? data_[at_++] : 0; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t at_{};
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) {
        return 0;
    }
    cursor ops{{data, size}};

    // The epoch matters: which level a deadline lands in depends on the absolute
    // tick, so a wheel anchored off a tick boundary exercises different slots.
    const auto epoch{std::chrono::nanoseconds{ops.next()} * 7919};

    libmem::arena arena{wheel::footprint()};
    auto w{wheel::carve(arena, tick, epoch)};
    if (!w) {
        return 0;
    }

    std::vector<std::pair<std::uint64_t, std::uint32_t>> reference{};
    std::vector<std::pair<dgram::timer_handle, std::uint32_t>> live{};
    std::uint32_t next_id{1};
    std::uint64_t now{};

    while (!ops.done()) {
        switch (ops.next() % 3) {
        case 0: { // schedule
            if (live.size() + 1 >= capacity) {
                break;
            }
            const std::uint64_t delay{1 + (ops.next() % (wheel::horizon - 1))};
            const auto id{next_id++};
            const auto h{w->schedule(id, epoch + tick * static_cast<std::int64_t>(now + delay))};
            if (!h.valid()) {
                std::abort(); // within capacity and horizon, so it must be taken
            }
            reference.emplace_back(now + delay, id);
            live.emplace_back(h, id);
            break;
        }
        case 1: { // cancel
            if (live.empty()) {
                break;
            }
            const auto pick{ops.next() % live.size()};
            const auto [handle, id]{live[pick]};
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(pick));
            if (w->cancel(handle)) {
                std::erase_if(reference, [id](const auto& p) { return p.second == id; });
            }
            break;
        }
        default: { // advance
            // Mostly short steps, which land on every cascade; sometimes a jump
            // across up to two horizons, which advance skips in one go.
            const auto step{ops.next()};
            now += (step & 0x80U) != 0 ? 1 + ((step & 0x7FU) * 4U) : 1 + (step % 9);

            std::vector<std::uint32_t> fired{};
            (void)w->advance(epoch + tick * static_cast<std::int64_t>(now), [&](const std::uint32_t id) { fired.push_back(id); });

            std::vector<std::uint32_t> expected{};
            for (const auto& [deadline, id] : reference) {
                if (deadline <= now) {
                    expected.push_back(id);
                }
            }
            std::erase_if(reference, [&](const auto& p) { return p.first <= now; });

            std::ranges::sort(fired);
            std::ranges::sort(expected);
            if (fired != expected) {
                std::abort(); // fired early, fired late, or lost a timer
            }
            std::erase_if(live, [&](const auto& p) { return std::ranges::binary_search(expected, p.second); });
            break;
        }
        }

        if (w->size() != reference.size()) {
            std::abort();
        }
    }

    // Nothing may be stranded: advancing past every deadline must drain the wheel.
    (void)w->advance(epoch + tick * static_cast<std::int64_t>(now + wheel::horizon));
    if (!w->empty()) {
        std::abort();
    }
    return 0;
}
