// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file doc_examples.cpp
 * @brief The examples printed in README.md and docs/, compiled and run.
 *
 * A snippet in a document is untested code that looks authoritative. Every
 * example the docs show is reproduced here so that it cannot quietly stop
 * compiling; if one is edited in a doc, it is edited here too.
 */
#include <gtest/gtest.h>

#include <sys/socket.h>

import std;
import libmem;
import dgram;

namespace {

using namespace std::chrono_literals;

/* ============================================================================
 * README: monadic error handling
 * ============================================================================ */

TEST(DocExamples, MonadicSetupChain) {
    const auto local{dgram::endpoint::any(dgram::family::inet4, 0)};

    auto sock{dgram::socket::open<dgram::reuse_addr>(dgram::family::inet4)};
    ASSERT_TRUE(sock.has_value());

    // Each step runs only if the previous one succeeded; the first error wins.
    const auto where = sock->bind(local)                                    //
                       | dgram::then([&] { return sock->local_address(); }) //
                       | dgram::map(&dgram::endpoint::text)                 //
                       | dgram::recover([](const dgram::errc e) {           //
                             return dgram::result<std::string>{std::string{dgram::describe(e)}};
                         });

    ASSERT_TRUE(where.has_value());
    EXPECT_TRUE(where->starts_with("0.0.0.0:"));
}

TEST(DocExamples, MonadicChainShortCircuitsAndRecovers) {
    // A parse that fails must skip every later step and land in recover.
    int steps_run{};
    const auto text = dgram::endpoint::parse(dgram::family::inet4, "not-an-address", 1) | dgram::tap([&](const auto&) { ++steps_run; }) |
                      dgram::map(&dgram::endpoint::text) |
                      dgram::recover([](const dgram::errc e) { return dgram::result<std::string>{std::string{dgram::describe(e)}}; });

    ASSERT_TRUE(text.has_value()) << "recover supplies a value";
    EXPECT_EQ(steps_run, 0) << "nothing after the failure ran";
    EXPECT_FALSE(text->empty());
}

/* ============================================================================
 * README: the at-a-glance example, verbatim
 * ============================================================================ */

TEST(DocExamples, AtAGlance) {
    using rx = dgram::receive_batch<64, 2048, dgram::features<dgram::ecn>>;
    using tx = dgram::transmit_batch<64>;

    // Setup is a chain: each step runs only if the last succeeded.
    auto sock{dgram::socket::open<dgram::reuse_addr, dgram::receive_metadata<dgram::ecn>>(dgram::family::inet4)};
    ASSERT_TRUE(sock.has_value());

    const auto listening = sock->bind(dgram::endpoint::any(dgram::family::inet4, 0)) //
                           | dgram::then([&] { return sock->local_address(); })      //
                           | dgram::map(&dgram::endpoint::text);
    ASSERT_TRUE(listening.has_value());

    libmem::arena arena{rx::footprint() + tx::footprint()};
    auto in{rx::carve(arena)};
    auto out{tx::carve(arena)};
    ASSERT_TRUE(in.has_value() && out.has_value());

    // Send something to ourselves so the loop below has work.
    {
        auto peer{dgram::socket::open<>(dgram::family::inet4)};
        ASSERT_TRUE(peer.has_value());
        const auto target{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", sock->local_address()->port())};
        constexpr std::string_view hello{"hello"};
        ::sendto(peer->native(), hello.data(), hello.size(), 0, target.raw(), target.size());
    }

    // The loop is a range pipeline. `segments()` splits a coalesced slot, so
    // nothing here branches on whether the kernel coalesced.
    ASSERT_TRUE(in->receive(*sock).has_value());
    std::size_t echoed{};
    for (const auto& d : in->datagrams() | std::views::filter(dgram::is_intact)) {
        for (const auto& piece : d.segments()) {
            if (out->stage(piece, d.from())) {
                ++echoed;
            }
        }
    }
    ASSERT_TRUE(out->flush(*sock).has_value());
    EXPECT_EQ(echoed, 1u);
}

/* ============================================================================
 * README: the truncation snippet (was stale: datagram is a template now)
 * ============================================================================ */

TEST(DocExamples, TruncationFilterCompiles) {
    using rx = dgram::receive_batch<8, 512>;
    using tx = dgram::transmit_batch<8, 512>;
    libmem::arena arena{rx::footprint() + tx::footprint()};
    auto batch{rx::carve(arena)};
    auto out{tx::carve(arena)};
    ASSERT_TRUE(batch.has_value() && out.has_value());

    // `is_intact` rather than a member pointer: datagram is templated on the
    // feature set, so &datagram<F>::intact would name that set at every call.
    for (const auto& d : batch->datagrams() | std::views::filter(dgram::is_intact)) {
        (void)out->stage(d.payload(), d.from());
    }
    SUCCEED();
}

} // namespace
