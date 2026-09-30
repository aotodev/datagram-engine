// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
// Batched UDP echo: one arena, one recvmmsg, one sendmmsg, no copy of the
// payload. Reflects each datagram's ECN marking back, reports the local address
// it arrived on, and splits GRO-coalesced slots back into datagrams.

#include <cstdio>
#include <cstdlib>

import std;
import libmem;
import dgram;

namespace {

constexpr std::size_t batch_capacity{16};

// A GRO slot can come back holding many datagrams, so it is sized for the
// coalesced buffer rather than for one MTU. Sizing it at an MTU would turn
// every coalesced receive into MSG_TRUNC.
constexpr std::size_t slot_bytes{1 << 16};

// The two directions carry different features: UDP_GRO is only ever received
// and UDP_SEGMENT only ever sent. ECN is read with `ecn` and sent with
// `traffic_class`, which carries the DSCP alongside it.
using rx_features = dgram::features<dgram::pktinfo, dgram::ecn, dgram::gro>;
using tx_features = dgram::features<dgram::traffic_class>;

using rx_batch = dgram::receive_batch<batch_capacity, slot_bytes, rx_features>;

// The transmit batch references the receive slots, so it carves no payload of
// its own; it still needs control space to attach the reflected ECN marking.
using tx_batch = dgram::transmit_batch<batch_capacity, 0, tx_features>;

constexpr std::size_t arena_bytes{rx_batch::footprint() + tx_batch::footprint()};

} // namespace

int main(const int argc, const char* const* argv) {
    const std::uint16_t port{argc > 1 ? static_cast<std::uint16_t>(std::atoi(argv[1])) : std::uint16_t{9000}};

    libmem::arena arena{arena_bytes};

    auto rx{rx_batch::carve(arena)};
    auto tx{tx_batch::carve(arena)};
    if (!rx || !tx) {
        std::println(stderr, "carve failed: arena too small");
        return 1;
    }

    auto sock{dgram::socket::open<dgram::reuse_port, dgram::recv_buffer<1 << 20>, dgram::receive_metadata<dgram::pktinfo, dgram::ecn>>(dgram::family::inet4)};
    if (!sock) {
        std::println(stderr, "open: {}", dgram::describe(sock.error()));
        return 1;
    }

    if (const auto bound{sock->bind(dgram::endpoint::any(dgram::family::inet4, port))}; !bound) {
        std::println(stderr, "bind: {}", dgram::describe(bound.error()));
        return 1;
    }

    std::println("echo listening on {} ({} B arena, {} B used, {} B control per datagram)", sock->local_address()->text(), arena.capacity(), arena.used(),
        rx_features::control_space);

    bool reported{false};

    while (true) {
        const auto got{rx->receive(*sock)};
        if (!got) {
            if (got.error() == dgram::interrupted) {
                continue;
            }
            std::println(stderr, "receive: {}", dgram::describe(got.error()));
            return 1;
        }

        // Bounce back everything that arrived whole. A truncated datagram means
        // slot_bytes is too small for this traffic, so it is reported, not echoed.
        const auto arrived{rx->datagrams()};
        for (const auto& d : arrived | std::views::filter(dgram::is_intact)) {
            const auto meta{d.meta()};

            // Reflect the sender's ECN codepoint rather than sending unmarked.
            dgram::control<tx_features> reply{};
            if (const auto marking{meta.get<dgram::ecn>()}) {
                reply.set<dgram::traffic_class>({.dscp = dgram::dscp::df, .ecn = *marking});
            }

            if (!reported) {
                if (const auto local{meta.get<dgram::pktinfo>()}) {
                    std::println("first datagram arrived on {} via interface {}", local->address.text(), local->interface);
                }
                reported = true;
            }

            // One slot may hold several datagrams when the kernel coalesced.
            // Iterating segments is correct either way, so there is nothing to
            // branch on here.
            for (const auto& piece : d.segments()) {
                if (!tx->stage(piece, d.from(), reply)) {
                    break;
                }
            }
        }

        if (const auto sent{tx->flush(*sock)}; !sent) {
            std::println(stderr, "flush: {}", dgram::describe(sent.error()));
        }
    }
}
