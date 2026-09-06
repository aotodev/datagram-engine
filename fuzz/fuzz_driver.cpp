// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file fuzz_driver.cpp
 * @brief `main` for the fuzz harness, in two flavours.
 *
 * Under `afl-g++-fast` this is AFL++ persistent mode: the process is forked once
 * and each test case is fed through shared memory, which is far faster than
 * exec-per-input and is what makes coverage-guided fuzzing practical here.
 *
 * Otherwise it is a standalone driver that replays any files given on the
 * command line and then mutates a seeded corpus under a fixed PRNG. That is
 * **not** coverage-guided and finds much less; its value is that it runs
 * wherever the library builds, under whichever sanitizer the build selected.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h> // AFL++'s persistent-mode macros expand to read()

import std;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

constexpr std::size_t default_iterations{200'000};
constexpr std::size_t max_input{2048};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/**
 * @brief One well-formed control message: `cmsg_len`, `cmsg_level`, `cmsg_type`,
 *        then `payload` bytes, padded to the alignment the macros expect.
 *
 * Without these the corpus never reaches a feature's `parse` at all: a random
 * byte stream essentially never produces a `cmsg_len` above `sizeof(cmsghdr)`
 * paired with a level and type a feature answers to. Seeding well-formed
 * messages puts mutation *around* the interesting boundary rather than
 * astronomically far from it.
 */
void append_cmsg(std::vector<std::uint8_t>& out, const int level, const int type, const std::size_t payload) {
    const auto len{CMSG_LEN(payload)};
    const auto space{CMSG_SPACE(payload)};
    const auto base{out.size()};
    out.resize(base + space, 0);

    ::cmsghdr header{};
    header.cmsg_len = len;
    header.cmsg_level = level;
    header.cmsg_type = type;
    std::memcpy(out.data() + base, &header, sizeof(header));
    for (std::size_t i{}; i < payload; ++i) {
        out[base + sizeof(::cmsghdr) + i] = static_cast<std::uint8_t>(0xA0 + i);
    }
}

/** A selector byte plus a body, which is the shape the harness expects. */
std::vector<std::uint8_t> seed(const std::uint8_t selector, auto&& fill) {
    std::vector<std::uint8_t> out{selector};
    fill(out);
    return out;
}

std::vector<std::vector<std::uint8_t>> builtin_seeds() {
    std::vector<std::vector<std::uint8_t>> seeds{
        {},
        {0},
        {0, 0},
        {1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        // a cmsg_len of zero, which must not advance the walk forever
        {0, 0, 0, 0, 0, 0, 0, 0, 0},
        // cmsg_len larger than the buffer that carries it
        {0, 0xf0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    };

    // Well-formed messages, at the widths the kernel really uses. Mutating the
    // length fields of these is what reaches the width-confusion bugs.
    for (std::uint8_t sel{}; sel < 3; ++sel) {
        seeds.push_back(seed(sel, [](auto& o) { append_cmsg(o, IPPROTO_IP, IP_TOS, 1); }));
        seeds.push_back(seed(sel, [](auto& o) { append_cmsg(o, IPPROTO_IPV6, IPV6_TCLASS, sizeof(int)); }));
        seeds.push_back(seed(sel, [](auto& o) { append_cmsg(o, IPPROTO_IP, IP_PKTINFO, sizeof(::in_pktinfo)); }));
        seeds.push_back(seed(sel, [](auto& o) { append_cmsg(o, IPPROTO_IPV6, IPV6_PKTINFO, sizeof(::in6_pktinfo)); }));
        // Both together, the shape a real dual-feature datagram carries.
        seeds.push_back(seed(sel, [](auto& o) {
            append_cmsg(o, IPPROTO_IP, IP_PKTINFO, sizeof(::in_pktinfo));
            append_cmsg(o, IPPROTO_IP, IP_TOS, 1);
        }));
        // The width the other family uses, on this family's level: the exact
        // confusion `parse` must survive.
        seeds.push_back(seed(sel, [](auto& o) { append_cmsg(o, IPPROTO_IP, IP_TOS, sizeof(int)); }));
        seeds.push_back(seed(sel, [](auto& o) { append_cmsg(o, IPPROTO_IPV6, IPV6_TCLASS, 1); }));
        seeds.push_back(seed(sel, [](auto& o) { append_cmsg(o, IPPROTO_IP, IP_PKTINFO, 1); }));
        seeds.push_back(seed(sel, [](auto& o) { append_cmsg(o, IPPROTO_IPV6, IPV6_PKTINFO, 1); }));
    }
    return seeds;
}

/** Byte-level mutations: the shape a coverage-guided engine would refine. */
void mutate(std::vector<std::uint8_t>& buf, std::mt19937_64& rng) {
    if (buf.empty() || (rng() % 8 == 0)) {
        buf.resize(std::min(max_input, static_cast<std::size_t>(rng() % max_input) + 1));
    }
    const auto edits{(rng() % 8) + 1};
    for (std::size_t i{}; i < edits; ++i) {
        const auto at{rng() % buf.size()};
        switch (rng() % 4) {
        case 0:
            buf[at] = static_cast<std::uint8_t>(rng());
            break;
        case 1:
            buf[at] = 0xff; // saturate a length field
            break;
        case 2:
            buf[at] = 0;
            break;
        default:
            buf[at] = static_cast<std::uint8_t>(buf[at] + 1);
            break;
        }
    }
}

} // namespace

#ifdef __AFL_COMPILER

/* AFL++ persistent mode. The corpus lives on disk and afl-fuzz drives it, so
   none of the seeding or mutation below is used. */
__AFL_FUZZ_INIT();

int main() {
    __AFL_INIT();
    const unsigned char* buf{__AFL_FUZZ_TESTCASE_BUF};
    while (__AFL_LOOP(100000)) {
        const int len{__AFL_FUZZ_TESTCASE_LEN};
        LLVMFuzzerTestOneInput(buf, static_cast<std::size_t>(len));
    }
    return 0;
}

#else

int main(const int argc, const char* const* argv) {
    auto corpus{builtin_seeds()};

    std::size_t replayed{};
    for (int i{1}; i < argc; ++i) {
        const std::filesystem::path path{argv[i]};
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        auto bytes{read_file(path)};
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
        corpus.push_back(std::move(bytes));
        ++replayed;
    }

    for (const auto& seed : corpus) {
        LLVMFuzzerTestOneInput(seed.data(), seed.size());
    }

    const char* env{std::getenv("DGRAM_FUZZ_ITERATIONS")};
    const auto iterations{env != nullptr ? std::strtoull(env, nullptr, 10) : default_iterations};

    std::mt19937_64 rng{0x9E3779B97F4A7C15ULL}; // fixed: a failure must reproduce
    std::vector<std::uint8_t> buf{};
    for (std::size_t i{}; i < iterations; ++i) {
        buf = corpus[rng() % corpus.size()];
        mutate(buf, rng);
        LLVMFuzzerTestOneInput(buf.data(), buf.size());
    }

    std::println("{}: {} corpus files replayed, {} mutated inputs, no crash", argc > 0 ? argv[0] : "fuzz", replayed, iterations);
    return 0;
}

#endif // __AFL_COMPILER
