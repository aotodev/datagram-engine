// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file fuzz_cmsg_parse.cpp
 * @brief Fuzzer for the ancillary-data parser.
 *
 * `parse_control` is the only place in the engine where attacker-controlled
 * bytes meet pointer arithmetic: the kernel writes the control buffer, the
 * `CMSG_*` macros walk it by lengths taken from the buffer itself, and each
 * feature then reads a payload whose width it must not assume.
 *
 * The input is interpreted as a selector byte followed by the raw control
 * buffer, which is copied into a `cmsghdr`-aligned block and handed to the
 * parser as if `recvmmsg` had just filled it. Nothing constrains `cmsg_len`, so
 * the interesting cases are reachable: zero, one, less than the header, larger
 * than `msg_controllen`, and values that overflow when advanced.
 *
 * Entry point is libFuzzer-shaped so `afl-g++-fast` can drive it coverage-guided.
 * `fuzz_driver.cpp` supplies a `main` for builds without a fuzzing engine.
 */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>

import std;
import dgram;

namespace {

using combined = dgram::features<dgram::pktinfo, dgram::ecn, dgram::traffic_class>;
using only_pktinfo = dgram::features<dgram::pktinfo>;
using only_ecn = dgram::features<dgram::ecn>;

constexpr std::size_t max_control{1024};

/**
 * @brief Run the parser over `bytes` as if the kernel had written them.
 *
 * The buffer is allocated to exactly the input length rather than reused from a
 * fixed-size block. That is the whole point: an overread of a few bytes stays
 * inside an oversized static array and no sanitizer ever sees it, whereas here
 * it lands in ASan's redzone immediately.
 */
template <typename Features> void parse_as(const std::span<const std::byte> bytes) noexcept {
    const auto len{std::min(bytes.size(), max_control)};
    if (len == 0) {
        return;
    }
    // Sized to the input exactly; `operator new` is over-aligned enough for a cmsghdr.
    auto* control{static_cast<std::byte*>(::operator new(len, std::align_val_t{alignof(::cmsghdr)}))};
    std::memcpy(control, bytes.data(), len);

    ::iovec io{};
    ::msghdr m{};
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = control;
    m.msg_controllen = len;

    const auto meta{dgram::detail::parse_control<Features>(m)};

    // Force the results to be observed so nothing is optimised away.
    if constexpr (Features::template contains<dgram::pktinfo>) {
        if (const auto& local{meta.template get<dgram::pktinfo>()}) {
            const auto sink{local->address.is_v4() || local->address.is_v6()};
            std::atomic_signal_fence(std::memory_order_acq_rel);
            (void)sink;
            (void)local->interface;
        }
    }
    if constexpr (Features::template contains<dgram::ecn>) {
        if (const auto& marking{meta.template get<dgram::ecn>()}) {
            (void)std::to_underlying(*marking);
        }
    }
    if constexpr (Features::template contains<dgram::traffic_class>) {
        if (const auto& marking{meta.template get<dgram::traffic_class>()}) {
            (void)marking->byte();
        }
    }

    ::operator delete(control, len, std::align_val_t{alignof(::cmsghdr)});
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const auto selector{data[0]};
    const std::span<const std::byte> body{reinterpret_cast<const std::byte*>(data) + 1, size - 1};

    switch (selector % 3) {
    case 0:
        parse_as<combined>(body);
        break;
    case 1:
        parse_as<only_pktinfo>(body);
        break;
    default:
        parse_as<only_ecn>(body);
        break;
    }
    return 0;
}
