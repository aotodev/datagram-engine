// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file malformed_control_tests.cpp
 * @brief Control buffers the kernel would never write, which the parser must
 *        still survive.
 *
 * Both cases here were found by `fuzz/fuzz_cmsg_parse.cpp` and are pinned as
 * unit tests so they hold without a fuzz run.
 */
#include <gtest/gtest.h>

#include <netinet/in.h>
#include <sys/socket.h>

import std;
import dgram;

namespace {

using both = dgram::features<dgram::pktinfo, dgram::ecn>;

/** A control buffer sized to exactly its contents, so any overread is detectable. */
class control_buffer {
public:
    explicit control_buffer(const std::size_t bytes) : bytes_{bytes}, storage_{::operator new(bytes, align)} {}
    control_buffer(const control_buffer&) = delete;
    control_buffer& operator=(const control_buffer&) = delete;
    ~control_buffer() { ::operator delete(storage_, bytes_, align); }

    [[nodiscard]] ::msghdr as_msghdr() noexcept {
        ::msghdr m{};
        m.msg_iov = &io_;
        m.msg_iovlen = 1;
        m.msg_control = storage_;
        m.msg_controllen = bytes_;
        return m;
    }

    [[nodiscard]] ::cmsghdr* header() noexcept { return static_cast<::cmsghdr*>(storage_); }
    [[nodiscard]] std::span<std::byte> bytes() noexcept { return {static_cast<std::byte*>(storage_), bytes_}; }

private:
    static constexpr std::align_val_t align{alignof(::cmsghdr)};
    std::size_t bytes_;
    void* storage_;
    ::iovec io_{};
};

/* `payload_size` is `cmsg_len - CMSG_LEN(0)` in size_t. A cmsg_len below the
   header size underflows to an enormous width, every caller's guard passes, and
   the read runs off the buffer. */
TEST(MalformedControl, LengthBelowTheHeaderDoesNotUnderflow) {
    control_buffer buf{CMSG_SPACE(sizeof(::in6_pktinfo))};
    std::ranges::fill(buf.bytes(), std::byte{0});

    auto* c{buf.header()};
    c->cmsg_level = IPPROTO_IPV6;
    c->cmsg_type = IPV6_PKTINFO;
    c->cmsg_len = 3; // nonsense: smaller than a cmsghdr

    EXPECT_EQ(dgram::detail::payload_size(c), 0u) << "must saturate, not wrap to SIZE_MAX";

    auto m{buf.as_msghdr()};
    const auto meta{dgram::detail::parse_control<both>(m)};
    EXPECT_FALSE(meta.get<dgram::pktinfo>().has_value());
}

/* CMSG_FIRSTHDR checks only that msg_controllen can hold a header, never that
   the header it returns is consistent with the buffer. */
TEST(MalformedControl, LengthOverrunningTheBufferIsRejected) {
    control_buffer buf{CMSG_SPACE(0) + 8};
    std::ranges::fill(buf.bytes(), std::byte{0});

    auto* c{buf.header()};
    c->cmsg_level = IPPROTO_IPV6;
    c->cmsg_type = IPV6_PKTINFO;
    c->cmsg_len = CMSG_LEN(sizeof(::in6_pktinfo)); // claims far more than is here

    auto m{buf.as_msghdr()};
    EXPECT_FALSE(dgram::detail::within_buffer(m, c)) << "a message must not claim more than the buffer holds";

    const auto meta{dgram::detail::parse_control<both>(m)};
    EXPECT_FALSE(meta.get<dgram::pktinfo>().has_value());
}

/* A truthful but short payload must be declined by the feature, not read wide. */
TEST(MalformedControl, ShortPayloadIsDeclinedNotWidened) {
    control_buffer buf{CMSG_SPACE(1)};
    std::ranges::fill(buf.bytes(), std::byte{0});

    auto* c{buf.header()};
    c->cmsg_level = IPPROTO_IPV6;
    c->cmsg_type = IPV6_PKTINFO;
    c->cmsg_len = CMSG_LEN(1); // honest, but one byte where 20 are needed

    auto m{buf.as_msghdr()};
    EXPECT_TRUE(dgram::detail::within_buffer(m, c));
    EXPECT_EQ(dgram::detail::payload_size(c), 1u);

    const auto meta{dgram::detail::parse_control<both>(m)};
    EXPECT_FALSE(meta.get<dgram::pktinfo>().has_value()) << "20 bytes cannot come from a 1-byte payload";
}

/* The widths really do differ per family, so ECN must accept both. */
TEST(MalformedControl, EcnAcceptsBothKernelWidths) {
    {
        control_buffer buf{CMSG_SPACE(1)};
        std::ranges::fill(buf.bytes(), std::byte{0});
        auto* c{buf.header()};
        c->cmsg_level = IPPROTO_IP;
        c->cmsg_type = IP_TOS;
        c->cmsg_len = CMSG_LEN(1);
        *reinterpret_cast<std::uint8_t*>(CMSG_DATA(c)) = 0x03;

        auto m{buf.as_msghdr()};
        const auto got{dgram::detail::parse_control<both>(m).get<dgram::ecn>()};
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(*got, dgram::ecn_codepoint::ce);
    }
    {
        control_buffer buf{CMSG_SPACE(sizeof(int))};
        std::ranges::fill(buf.bytes(), std::byte{0});
        auto* c{buf.header()};
        c->cmsg_level = IPPROTO_IPV6;
        c->cmsg_type = IPV6_TCLASS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        const int value{0x02};
        std::memcpy(CMSG_DATA(c), &value, sizeof(value));

        auto m{buf.as_msghdr()};
        const auto got{dgram::detail::parse_control<both>(m).get<dgram::ecn>()};
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(*got, dgram::ecn_codepoint::ect0);
    }
}

/* An empty control buffer is the common case for a socket with nothing enabled. */
TEST(MalformedControl, EmptyBufferParsesToNothing) {
    ::msghdr m{};
    const auto meta{dgram::detail::parse_control<both>(m)};
    EXPECT_FALSE(meta.get<dgram::pktinfo>().has_value());
    EXPECT_FALSE(meta.get<dgram::ecn>().has_value());
}

} // namespace
