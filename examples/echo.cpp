// Batched UDP echo: one arena, one recvmmsg, one sendmmsg, no copy of the payload.

#include <cstdio>
#include <cstdlib>

import std;
import libmem;
import dgram;

namespace {

constexpr std::size_t batch_capacity{64};
constexpr std::size_t slot_bytes{2048};

using rx_batch = dgram::receive_batch<batch_capacity, slot_bytes>;
using tx_batch = dgram::transmit_batch<batch_capacity>;

// The transmit batch references the receive slots, so it carves no payload of its own.
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

    auto sock{dgram::socket::open<dgram::reuse_port, dgram::recv_buffer<1 << 20>>(dgram::family::inet4)};
    if (!sock) {
        std::println(stderr, "open: {}", dgram::describe(sock.error()));
        return 1;
    }

    if (const auto bound{sock->bind(dgram::endpoint::any(dgram::family::inet4, port))}; !bound) {
        std::println(stderr, "bind: {}", dgram::describe(bound.error()));
        return 1;
    }

    std::println("echo listening on {} ({} B arena, {} B used)", sock->local_address()->text(), arena.capacity(), arena.used());

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
        for (const auto& d : rx->datagrams() | std::views::filter(&dgram::datagram::intact)) {
            if (!tx->stage(d.payload(), d.from())) {
                break;
            }
        }

        if (const auto sent{tx->flush(*sock)}; !sent) {
            std::println(stderr, "flush: {}", dgram::describe(sent.error()));
        }
    }
}
