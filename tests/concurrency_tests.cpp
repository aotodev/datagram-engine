// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file concurrency_tests.cpp
 * @brief The shared-nothing claim, made executable.
 *
 * The engine's threading model is one socket, one arena and one batch pair per
 * thread behind `SO_REUSEPORT`, sharing nothing. Nothing in the library is
 * synchronised, so that claim is only worth what a race detector says about it.
 * Run this under `-DDGRAM_SANITIZER=thread`.
 */
#include <gtest/gtest.h>

import std;
import libmem;
import dgram;

namespace {

constexpr std::size_t capacity{16};
constexpr std::size_t slot{512};
constexpr std::size_t worker_count{4};
constexpr std::size_t datagrams_per_worker{64};

using meta_set = dgram::features<dgram::pktinfo, dgram::ecn>;
using tx_set = dgram::features<dgram::pktinfo, dgram::traffic_class>;
using rx_batch = dgram::receive_batch<capacity, slot, meta_set>;
using tx_batch = dgram::transmit_batch<capacity, slot, tx_set>;

std::span<const std::byte> bytes_of(std::string_view s) noexcept {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

/* Every worker owns its arena, its socket and its batches outright; the only
   thing crossing a thread boundary is the atomic counter this test uses to
   observe progress. If the engine held any shared state, TSan would report it. */
TEST(Concurrency, WorkersShareNothing) {
    std::atomic<std::size_t> echoed{};
    std::atomic<bool> go{false};
    std::vector<std::jthread> workers{};
    workers.reserve(worker_count);

    for (std::size_t w{}; w < worker_count; ++w) {
        // `w` by value: capturing the loop variable by reference reads it after
        // the loop has moved on, which is a use-after-scope the moment the
        // thread outlives the iteration.
        workers.emplace_back([&, w] {
            libmem::arena arena{rx_batch::footprint() + tx_batch::footprint()};

            auto listener{
                dgram::socket::open<dgram::reuse_addr, dgram::nonblocking, dgram::receive_metadata<dgram::pktinfo, dgram::ecn>>(dgram::family::inet4)};
            ASSERT_TRUE(listener.has_value());
            ASSERT_TRUE(listener->bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());

            auto peer{dgram::socket::open<>(dgram::family::inet4)};
            ASSERT_TRUE(peer.has_value());
            const auto target{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", listener->local_address()->port())};

            auto rx{rx_batch::carve(arena)};
            auto tx{tx_batch::carve(arena)};
            ASSERT_TRUE(rx.has_value() && tx.has_value());

            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t sent{}; sent < datagrams_per_worker; ++sent) {
                dgram::control<tx_set> marked{};
                marked.set<dgram::traffic_class>({.ecn = dgram::ecn_codepoint::ect0});
                const auto text{std::format("w-{}-{}", w, sent)};
                if (!tx->stage_copy(bytes_of(text), target, marked)) {
                    continue;
                }
                ASSERT_TRUE(tx->flush(*peer).drained());

                // Nonblocking, so poll briefly rather than parking a thread.
                for (int attempt{}; attempt < 2000; ++attempt) {
                    const auto got{rx->receive(*listener)};
                    if (got.has_value()) {
                        const auto view{rx->datagrams()};
                        for (const auto& d : view | std::views::filter(dgram::is_intact)) {
                            const auto meta{d.meta()};
                            EXPECT_TRUE(meta.get<dgram::pktinfo>().has_value());
                            EXPECT_EQ(*meta.get<dgram::ecn>(), dgram::ecn_codepoint::ect0);
                            echoed.fetch_add(1, std::memory_order_relaxed);
                        }
                        break;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }

    go.store(true, std::memory_order_release);
    workers.clear(); // join

    EXPECT_GT(echoed.load(), 0u) << "no datagram completed a round trip";
    EXPECT_LE(echoed.load(), worker_count * datagrams_per_worker);
}

/* Two batches carved from one arena must not overlap: the receive path writes
   through both concurrently in a split I/O design. */
TEST(Concurrency, BatchesFromOneArenaDoNotOverlap) {
    libmem::arena arena{rx_batch::footprint() + tx_batch::footprint()};
    auto a{rx_batch::carve(arena)};
    auto b{rx_batch::carve(arena)};
    ASSERT_TRUE(a.has_value());
    // The arena is sized for one receive and one transmit batch, so a second
    // receive batch must fail rather than alias the first.
    if (b.has_value()) {
        const auto first{a->slot(0)};
        const auto second{b->slot(0)};
        EXPECT_TRUE(first.data() + first.size() <= second.data() || second.data() + second.size() <= first.data())
            << "two carved batches must not share payload memory";
    }
}

} // namespace
