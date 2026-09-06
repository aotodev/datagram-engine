// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file fuzz_segments.cpp
 * @brief Fuzzer for the GRO segmentation walk.
 *
 * The stride comes from a `UDP_GRO` control message, so it is as
 * kernel-supplied as the buffer is, and the walk is pure offset arithmetic over
 * a span. The properties checked here are the ones a wrong stride would break:
 * every segment lies inside the buffer, the segments are contiguous and ordered,
 * and together they cover the payload exactly once.
 *
 * The buffer is allocated to the input length so an off-by-one lands in a
 * redzone rather than inside slack.
 */
#include <cstddef>
#include <cstdint>
#include <cstring>

import std;
import dgram;

namespace {

constexpr std::size_t max_bytes{1 << 16};

void check(const std::span<const std::byte> bytes, const std::size_t stride) noexcept {
    std::size_t covered{};
    std::size_t count{};
    const std::byte* previous_end{bytes.data()};

    for (const auto& piece : dgram::segments_of(bytes, stride)) {
        ++count;
        // Inside the buffer.
        if (piece.data() < bytes.data() || piece.data() + piece.size() > bytes.data() + bytes.size()) {
            std::abort();
        }
        // Contiguous with what came before, in order.
        if (piece.data() != previous_end) {
            std::abort();
        }
        previous_end = piece.data() + piece.size();
        covered += piece.size();
        if (count > max_bytes + 1) {
            std::abort(); // the walk must terminate
        }
    }

    if (covered != bytes.size()) {
        std::abort(); // every byte belongs to exactly one segment
    }
    if (count == 0) {
        std::abort(); // even an empty datagram is one datagram
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) {
        return 0;
    }
    // First two bytes are the stride, exactly as a uint16 UDP_GRO would give.
    std::uint16_t stride{};
    std::memcpy(&stride, data, sizeof(stride));

    const auto len{std::min(size - 2, max_bytes)};
    if (len == 0) {
        check({}, stride);
        return 0;
    }

    auto* buf{static_cast<std::byte*>(::operator new(len))};
    std::memcpy(buf, data + 2, len);
    check({buf, len}, stride);
    ::operator delete(buf, len);
    return 0;
}
