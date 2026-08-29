/**
 * @file offload_tests.cpp
 * @brief GRO receive and GSO transmit over loopback, plus the segmentation walk.
 */
#include <gtest/gtest.h>

#include <netinet/in.h>
#include <netinet/udp.h>
#include <sys/socket.h>

import std;
import libmem;
import dgram;

namespace {

constexpr std::uint16_t mtu{1400};
constexpr std::size_t capacity{8};

/* A GRO socket hands back many datagrams in one slot, so the slot has to hold
   the coalesced buffer rather than one MTU. */
constexpr std::size_t gro_slot{1 << 16};

using rx_set = dgram::features<dgram::gro, dgram::ecn>;
using tx_set = dgram::features<dgram::segment, dgram::ecn>;
using rx_batch = dgram::receive_batch<capacity, gro_slot, rx_set>;
using tx_batch = dgram::transmit_batch<capacity, gro_slot, tx_set>;

std::span<const std::byte> bytes_of(std::string_view s) noexcept {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

/* ============================================================================
 * The segmentation walk, independent of any socket
 * ============================================================================ */

std::vector<std::size_t> sizes_of(auto&& view) {
    std::vector<std::size_t> out{};
    for (const auto& s : view) {
        out.push_back(s.size());
    }
    return out;
}

TEST(Segments, ShortFinalSegment) {
    std::array<std::byte, 4400> buf{};
    EXPECT_EQ(sizes_of(dgram::segments_of(buf, 1400)), (std::vector<std::size_t>{1400, 1400, 1400, 200}));
}

TEST(Segments, ExactMultipleHasNoEmptyTail) {
    std::array<std::byte, 4200> buf{};
    EXPECT_EQ(sizes_of(dgram::segments_of(buf, 1400)), (std::vector<std::size_t>{1400, 1400, 1400}));
}

TEST(Segments, StrideZeroMeansOneDatagram) {
    std::array<std::byte, 500> buf{};
    EXPECT_EQ(sizes_of(dgram::segments_of(buf, 0)), (std::vector<std::size_t>{500}));
}

TEST(Segments, StrideLargerThanBufferMeansOneDatagram) {
    std::array<std::byte, 300> buf{};
    EXPECT_EQ(sizes_of(dgram::segments_of(buf, 1400)), (std::vector<std::size_t>{300}));
}

/* A zero-length UDP datagram is legal. Yielding nothing would lose it. */
TEST(Segments, EmptyPayloadIsOneEmptyDatagram) {
    EXPECT_EQ(sizes_of(dgram::segments_of({}, 1400)), (std::vector<std::size_t>{0}));
    EXPECT_EQ(sizes_of(dgram::segments_of({}, 0)), (std::vector<std::size_t>{0}));
}

TEST(Segments, SegmentsCoverThePayloadExactlyAndInOrder) {
    std::array<std::byte, 4400> buf{};
    for (auto [i, b] : std::views::enumerate(buf)) {
        b = static_cast<std::byte>(i & 0xFF);
    }
    std::size_t seen{};
    for (const auto& s : dgram::segments_of(buf, 1400)) {
        EXPECT_EQ(s.data(), buf.data() + seen) << "segments must be contiguous and ordered";
        seen += s.size();
    }
    EXPECT_EQ(seen, buf.size()) << "every byte belongs to exactly one segment";
}

TEST(Segments, ViewIsLazyAndAllocationFree) {
    std::array<std::byte, 4400> buf{};
    const auto view{dgram::segments_of(buf, 1400)};
    static_assert(std::ranges::random_access_range<decltype(view)>);
    static_assert(std::is_trivially_copyable_v<decltype(view.begin())>);
    EXPECT_EQ(std::ranges::size(view), 4u);
}

/* ============================================================================
 * Over the wire
 * ============================================================================ */

struct fixture {
    libmem::arena arena{rx_batch::footprint() + tx_batch::footprint()};
    dgram::socket receiver{
        std::move(*dgram::socket::open<dgram::reuse_addr, dgram::recv_buffer<1 << 21>, dgram::receive_metadata<dgram::gro, dgram::ecn>>(dgram::family::inet4))};
    dgram::socket sender{std::move(*dgram::socket::open<>(dgram::family::inet4))};
    dgram::endpoint target{};

    fixture() {
        EXPECT_TRUE(receiver.bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());
        target = *dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", receiver.local_address()->port());
    }
};

/* One sendmmsg entry carrying many datagrams, coming back as one recvmmsg slot
   that splits into the same datagrams. */
TEST(Offload, GsoSendCoalescesIntoOneGroSlot) {
    fixture net{};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    constexpr std::size_t full_segments{4};
    constexpr std::size_t tail{200};
    std::vector<std::byte> payload(full_segments * mtu + tail);
    for (auto [i, b] : std::views::enumerate(payload)) {
        b = static_cast<std::byte>((i / mtu) & 0xFF);
    }

    dgram::control<tx_set> ancillary{};
    ancillary.set<dgram::segment>(mtu);
    ASSERT_TRUE(tx->stage(payload, net.target, ancillary));
    const auto sent{tx->flush(net.sender)};
    ASSERT_TRUE(sent.has_value()) << dgram::describe(sent.error());

    ASSERT_EQ(*rx->receive(net.receiver), 1u) << "GRO should deliver the lot in one slot";

    const auto view{rx->datagrams()};
    const auto d{*view.begin()};
    ASSERT_TRUE(d.intact());
    EXPECT_EQ(d.payload().size(), payload.size());

    const auto size{d.meta().get<dgram::gro>()};
    ASSERT_TRUE(size.has_value()) << "the kernel coalesced, so it must report the segment size";
    EXPECT_EQ(*size, mtu);

    const auto pieces{sizes_of(d.segments())};
    ASSERT_EQ(pieces.size(), full_segments + 1);
    for (std::size_t i{}; i < full_segments; ++i) {
        EXPECT_EQ(pieces[i], mtu);
    }
    EXPECT_EQ(pieces.back(), tail) << "the final segment is whatever remained";

    // Content must survive the round trip segment for segment.
    std::size_t offset{};
    for (const auto& piece : d.segments()) {
        EXPECT_TRUE(std::ranges::equal(piece, std::span{payload}.subspan(offset, piece.size())));
        offset += piece.size();
    }
    EXPECT_EQ(offset, payload.size());
}

/* Without coalescing there is no control message, and segments() must still
   yield the one datagram rather than nothing. */
TEST(Offload, UncoalescedDatagramIsOneSegment) {
    fixture net{};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    ASSERT_TRUE(tx->stage_copy(bytes_of("a single small datagram"), net.target));
    ASSERT_TRUE(tx->flush(net.sender).has_value());

    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    const auto view{rx->datagrams()};
    const auto d{*view.begin()};

    EXPECT_FALSE(d.meta().get<dgram::gro>().has_value()) << "nothing was coalesced, so nothing is reported";
    const auto pieces{sizes_of(d.segments())};
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_EQ(pieces.front(), std::string_view{"a single small datagram"}.size());
}

/* GSO and ECN on the same datagram: the control block carries both. */
TEST(Offload, SegmentAndEcnTravelTogether) {
    fixture net{};
    auto rx{rx_batch::carve(net.arena)};
    auto tx{tx_batch::carve(net.arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    std::vector<std::byte> payload(3 * mtu);
    dgram::control<tx_set> ancillary{};
    ancillary.set<dgram::segment>(mtu);
    ancillary.set<dgram::ecn>(dgram::ecn_codepoint::ect0);

    ASSERT_TRUE(tx->stage(payload, net.target, ancillary));
    ASSERT_TRUE(tx->flush(net.sender).has_value());

    ASSERT_EQ(*rx->receive(net.receiver), 1u);
    const auto view{rx->datagrams()};
    const auto meta{(*view.begin()).meta()};
    EXPECT_EQ(*meta.get<dgram::gro>(), mtu);
    ASSERT_TRUE(meta.get<dgram::ecn>().has_value());
    EXPECT_EQ(*meta.get<dgram::ecn>(), dgram::ecn_codepoint::ect0);
}

/* A GRO slot sized for one MTU truncates, and must say so. */
TEST(Offload, UndersizedSlotReportsTruncation) {
    using tiny_rx = dgram::receive_batch<capacity, mtu, rx_set>;
    libmem::arena arena{tiny_rx::footprint() + tx_batch::footprint()};

    dgram::socket receiver{std::move(*dgram::socket::open<dgram::reuse_addr, dgram::receive_metadata<dgram::gro, dgram::ecn>>(dgram::family::inet4))};
    dgram::socket sender{std::move(*dgram::socket::open<>(dgram::family::inet4))};
    ASSERT_TRUE(receiver.bind(dgram::endpoint::any(dgram::family::inet4, 0)).has_value());
    const auto target{*dgram::endpoint::parse(dgram::family::inet4, "127.0.0.1", receiver.local_address()->port())};

    auto rx{tiny_rx::carve(arena)};
    auto tx{tx_batch::carve(arena)};
    ASSERT_TRUE(rx.has_value() && tx.has_value());

    std::vector<std::byte> payload(4 * mtu);
    dgram::control<tx_set> ancillary{};
    ancillary.set<dgram::segment>(mtu);
    ASSERT_TRUE(tx->stage(payload, target, ancillary));
    ASSERT_TRUE(tx->flush(sender).has_value());

    ASSERT_EQ(*rx->receive(receiver), 1u);
    const auto view{rx->datagrams()};
    const auto d{*view.begin()};
    EXPECT_TRUE(d.truncated()) << "a slot sized for one MTU cannot hold a coalesced buffer";
    EXPECT_FALSE(d.intact());
}

} // namespace
