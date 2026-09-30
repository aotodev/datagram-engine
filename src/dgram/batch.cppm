// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file batch.cppm
 * @brief Pre-carved `recvmmsg` / `sendmmsg` argument blocks and the views over them.
 */
module;

#include <cassert>
#include <cerrno>
#include <limits.h>
#include <sys/socket.h>

export module dgram:batch;

import std;
import libmem;

import :address;
import :cmsg;
import :error;
import :feature;
import :offload;
import :socket;

namespace dgram {

/**
 * @brief Payload slots start on their own cache line, so adjacent slots never share one.
 *
 * Not `std::hardware_destructive_interference_size`: this value feeds the
 * exported `footprint()`, and GCC warns that the standard constant is not ABI
 * stable. A caller compiled with a different `--param` would compute a different
 * footprint for the same batch type.
 */
export inline constexpr std::size_t slot_alignment{64};

/* ============================================================================
 * Geometry
 * ============================================================================ */

/** @brief The byte counts a batch needs, known before anything is allocated. */
export struct layout {
    std::size_t capacity;      ///< datagrams per syscall
    std::size_t slot_bytes;    ///< payload bytes per datagram
    std::size_t control_bytes; ///< ancillary-data bytes per datagram

    /**
     * @brief Upper bound on the bytes a batch of this shape draws from a resource.
     *
     * Deliberately conservative: every block is costed with its worst-case
     * leading padding, because a caller sizing an arena from this must never
     * come up short.
     */
    [[nodiscard]] constexpr std::size_t bytes() const noexcept {
        constexpr auto block = [](const std::size_t n, const std::size_t align) constexpr noexcept { return n == 0 ? 0 : n + align - 1; };
        return block(capacity * sizeof(::mmsghdr), alignof(::mmsghdr)) //
               + block(capacity * sizeof(::iovec), alignof(::iovec))   //
               + block(capacity * sizeof(endpoint), alignof(endpoint)) //
               + block(capacity * control_bytes, alignof(::cmsghdr))   //
               + block(capacity * slot_bytes, slot_alignment);
    }
};

/* ============================================================================
 * Received datagrams
 * ============================================================================ */

/**
 * @brief One received datagram, borrowed from the batch that received it.
 *
 * Valid until the next `receive` on that batch. A protocol that needs the bytes
 * for longer copies them.
 */
export template <feature_set Features = no_features> class datagram {
public:
    explicit constexpr datagram(const ::mmsghdr& raw) noexcept : raw_{&raw} {}

    /** @brief The received bytes, never longer than the slot. */
    [[nodiscard]] std::span<const std::byte> payload() const noexcept {
        // A caller-supplied MSG_TRUNC makes msg_len the wire length, not what was copied.
        const auto& iov{raw_->msg_hdr.msg_iov[0]};
        return {static_cast<const std::byte*>(iov.iov_base), std::min<std::size_t>(raw_->msg_len, iov.iov_len)};
    }

    [[nodiscard]] const endpoint& from() const noexcept { return *static_cast<const endpoint*>(raw_->msg_hdr.msg_name); }

    [[nodiscard]] constexpr int flags() const noexcept { return raw_->msg_hdr.msg_flags; }

    /**
     * @brief Parse this datagram's ancillary data.
     *
     * Walks the control buffer on each call rather than eagerly after `receive`,
     * so a caller that never asks pays nothing. Bind the result if you need it
     * more than once.
     */
    [[nodiscard]] metadata<Features> meta() const noexcept { return detail::parse_control<Features>(raw_->msg_hdr); }

    /**
     * @brief The datagrams packed into this slot, as a lazy view of spans.
     *
     * With `gro` in the feature set and the kernel having coalesced, one slot
     * holds several datagrams and this splits them; otherwise it yields the
     * whole payload as one. Either way iterating this is correct, so a receive
     * loop need not branch on whether offload is in play.
     *
     * Parses the control buffer to find the segment size. A caller that already
     * holds a `meta()` should pass the size to `segments_of` instead of walking
     * the buffer twice.
     */
    [[nodiscard]] auto segments() const noexcept {
        std::size_t stride{};
        if constexpr (Features::template contains<gro>) {
            if (const auto size{meta().template get<gro>()}) {
                stride = *size;
            }
        }
        return segments_of(payload(), stride);
    }

    /**
     * @brief Payload did not fit the slot and the excess is gone.
     *
     * The kernel reports this no other way, so ignoring it turns an undersized
     * `slot_bytes` into silent corruption.
     */
    [[nodiscard]] constexpr bool truncated() const noexcept { return (flags() & MSG_TRUNC) != 0; }

    /** @brief Ancillary data did not fit: the control buffer is undersized for the feature set. */
    [[nodiscard]] constexpr bool control_truncated() const noexcept { return (flags() & MSG_CTRUNC) != 0; }

    /** @brief Neither payload nor control data was lost. */
    [[nodiscard]] constexpr bool intact() const noexcept { return (flags() & (MSG_TRUNC | MSG_CTRUNC)) == 0; }

private:
    const ::mmsghdr* raw_;
};

/**
 * @brief Predicates over a datagram, usable as range adaptors.
 *
 * `datagram` is templated on the feature set, so a member pointer would have to
 * name that set at every call site. These do not.
 */
export inline constexpr auto is_intact = [](const auto& d) static noexcept { return d.intact(); };
export inline constexpr auto is_truncated = [](const auto& d) static noexcept { return d.truncated(); };
export inline constexpr auto is_control_truncated = [](const auto& d) static noexcept { return d.control_truncated(); };

namespace detail {

/** @brief Stateless projection from a kernel-filled `mmsghdr` to a `datagram`. */
template <feature_set Features> struct to_datagram_fn {
    [[nodiscard]] constexpr datagram<Features> operator()(const ::mmsghdr& m) const noexcept { return datagram<Features>{m}; }
};

template <feature_set Features> inline constexpr to_datagram_fn<Features> to_datagram{};

/**
 * @brief Carve `count` objects out of `r`, empty if it is exhausted.
 *
 * Never paired with a `deallocate`, which is what `aligned_monotonic_resource`
 * requires of `R`: the caller's resource reclaims every block at once.
 */
template <typename T, libmem::aligned_monotonic_resource R>
[[nodiscard]] std::span<T> carve_span(R& r, const std::size_t count, const std::size_t align = alignof(T)) noexcept {
    if (count == 0) {
        return {};
    }
    void* mem{r.allocate(count * sizeof(T), align)};
    if (!mem) [[unlikely]] {
        return {};
    }
    return std::span<T>{static_cast<T*>(mem), count};
}

} // namespace detail

/* ============================================================================
 * Receive
 * ============================================================================ */

/**
 * @brief Fixed-size `recvmmsg` argument block over caller-provided memory.
 *
 * Not thread-safe and not meant to be: the engine's model is one batch per
 * thread behind `SO_REUSEPORT`, so a batch never crosses a thread boundary.
 */
export template <std::size_t Capacity, std::size_t SlotBytes, feature_set Features = no_features> class receive_batch {
    static_assert(Capacity > 0 && Capacity <= IOV_MAX, "capacity must fit IOV_MAX");
    static_assert(SlotBytes > 0, "a receive slot needs room for a datagram");

public:
    using features_type = Features;

    static constexpr layout geometry{
        .capacity = Capacity,
        .slot_bytes = SlotBytes,
        .control_bytes = Features::control_space,
    };

    /** @brief Bytes this batch draws from a resource. */
    static constexpr std::size_t footprint() noexcept { return geometry.bytes(); }

    /**
     * @brief Carve the whole batch out of `resource` and wire the headers.
     *
     * Every allocation the batch will ever make happens here. After this
     * returns, the receive path allocates nothing.
     *
     * The batch borrows and never frees, so `resource` must outlive it and must
     * reclaim in bulk. That is what `aligned_monotonic_resource` asks for: an
     * arena, or a caller's own bump allocator that opts in.
     */
    template <libmem::aligned_monotonic_resource R> [[nodiscard]] static result<receive_batch> carve(R& resource) noexcept {
        receive_batch b{};
        b.msgs_ = detail::carve_span<::mmsghdr>(resource, Capacity);
        b.iovs_ = detail::carve_span<::iovec>(resource, Capacity);
        b.addrs_ = detail::carve_span<endpoint>(resource, Capacity);
        b.control_ = detail::carve_span<std::byte>(resource, Capacity * Features::control_space, alignof(::cmsghdr));
        b.payload_ = detail::carve_span<std::byte>(resource, Capacity * SlotBytes, slot_alignment);

        const bool complete{
            !b.msgs_.empty() && !b.iovs_.empty() && !b.addrs_.empty() && !b.payload_.empty() && (Features::control_space == 0 || !b.control_.empty())};
        if (!complete) [[unlikely]] {
            return std::unexpected{out_of_memory};
        }

        std::ranges::uninitialized_value_construct(b.addrs_);
        b.wire();
        return b;
    }

    /**
     * @brief Receive up to `Capacity` datagrams in one syscall.
     *
     * Rearms the headers first, so a batch is always safe to reuse. Any view
     * from a previous `receive` dangles once this is called.
     *
     * `MSG_WAITFORONE` is always set. A blocking `recvmmsg` without it waits for
     * all `Capacity` datagrams, so a batch of 64 would stall until 64 arrive:
     * unbounded added latency, and a deadlock outright for any traffic that
     * comes in bursts smaller than the batch. With it, the call returns as soon
     * as one datagram is there and takes whatever else is already queued.
     */
    [[nodiscard]] result<std::size_t> receive(const socket& sock, const int flags = 0) noexcept {
        rearm(received_);
        const int n{::recvmmsg(sock.native(), msgs_.data(), static_cast<unsigned>(Capacity), flags | MSG_WAITFORONE, nullptr)};
        if (n < 0) [[unlikely]] {
            received_ = 0;
            return fail<std::size_t>();
        }
        received_ = static_cast<std::size_t>(n);
        return received_;
    }

    /** @brief Lazy view over the datagrams the last `receive` produced. */
    [[nodiscard]] auto datagrams() const noexcept {
        return std::span<const ::mmsghdr>{msgs_.data(), received_} | std::views::transform(detail::to_datagram<Features>);
    }

    [[nodiscard]] constexpr std::size_t received() const noexcept { return received_; }

    /**
     * @brief The raw slot behind index `i`, for a caller driving this batch itself.
     *
     * **Not reachable from a `route` sink.** An `arrival` carries no index and
     * nothing else identifying the slot its bytes came from, so a protocol
     * behind `route` cannot get here and copies instead. This is for a caller
     * that walks `datagrams()` on its own and still knows which index it is on.
     *
     * What copying instead costs has been measured; see docs/batches.md.
     */
    [[nodiscard]] std::span<std::byte> slot(const std::size_t i) noexcept {
        assert(i < Capacity);
        return payload_.subspan(i * SlotBytes, SlotBytes);
    }

private:
    receive_batch() noexcept = default;

    /** @brief One-time wiring of the pointers that never change. */
    void wire() noexcept {
        for (auto [i, m] : std::views::enumerate(msgs_)) {
            const auto idx{static_cast<std::size_t>(i)};
            iovs_[idx].iov_base = payload_.data() + (idx * SlotBytes);
            iovs_[idx].iov_len = SlotBytes;

            m.msg_hdr = ::msghdr{};
            m.msg_hdr.msg_name = &addrs_[idx];
            m.msg_hdr.msg_iov = &iovs_[idx];
            m.msg_hdr.msg_iovlen = 1;
            if constexpr (Features::control_space > 0) {
                m.msg_hdr.msg_control = control_.data() + (idx * Features::control_space);
            }
        }
        rearm(Capacity);
    }

    /**
     * @brief Restore the fields `recvmmsg` overwrites with actuals.
     *
     * `msg_namelen` and `msg_controllen` come back holding what was *received*,
     * not the capacity supplied. Reusing the block without restoring them makes
     * the next call truncate silently, which is the trap that comes with wiring
     * the headers only once. `iov_len` is restored alongside them because a
     * caller that retained a slot may have shortened it.
     */
    void rearm(const std::size_t count) noexcept {
        for (auto& m : std::span{msgs_.data(), count}) {
            m.msg_hdr.msg_namelen = sizeof(::sockaddr_storage);
            m.msg_hdr.msg_controllen = Features::control_space;
            m.msg_hdr.msg_flags = 0;
            m.msg_hdr.msg_iov[0].iov_len = SlotBytes;
            m.msg_len = 0;
        }
    }

    std::span<::mmsghdr> msgs_{};
    std::span<::iovec> iovs_{};
    std::span<endpoint> addrs_{};
    std::span<std::byte> control_{};
    std::span<std::byte> payload_{};
    std::size_t received_{};
};

/* ============================================================================
 * Transmit
 * ============================================================================ */

/**
 * @brief Fixed-size `sendmmsg` argument block over caller-provided memory.
 *
 * `stage` references the caller's bytes instead of copying them, so echoing
 * straight out of a `receive_batch` slot costs nothing. The referenced bytes
 * must stay put until `flush` returns. `SlotBytes` of zero carves no payload
 * memory at all and leaves only the referencing path available.
 */
export template <std::size_t Capacity, std::size_t SlotBytes = 0, feature_set Features = no_features> class transmit_batch {
    static_assert(Capacity > 0 && Capacity <= IOV_MAX, "capacity must fit IOV_MAX");

public:
    using features_type = Features;

    static constexpr layout geometry{
        .capacity = Capacity,
        .slot_bytes = SlotBytes,
        .control_bytes = Features::control_space,
    };

    static constexpr std::size_t footprint() noexcept { return geometry.bytes(); }

    template <libmem::aligned_monotonic_resource R> [[nodiscard]] static result<transmit_batch> carve(R& resource) noexcept {
        transmit_batch b{};
        b.msgs_ = detail::carve_span<::mmsghdr>(resource, Capacity);
        b.iovs_ = detail::carve_span<::iovec>(resource, Capacity);
        b.addrs_ = detail::carve_span<endpoint>(resource, Capacity);
        b.control_ = detail::carve_span<std::byte>(resource, Capacity * Features::control_space, alignof(::cmsghdr));
        b.payload_ = detail::carve_span<std::byte>(resource, Capacity * SlotBytes, slot_alignment);

        const bool complete{!b.msgs_.empty() && !b.iovs_.empty() && !b.addrs_.empty() && (Features::control_space == 0 || !b.control_.empty()) &&
                            (SlotBytes == 0 || !b.payload_.empty())};
        if (!complete) [[unlikely]] {
            return std::unexpected{out_of_memory};
        }

        std::ranges::uninitialized_value_construct(b.addrs_);
        b.wire();
        return b;
    }

    /** @brief Whether another datagram fits before a flush is required. */
    [[nodiscard]] constexpr bool full() const noexcept { return staged_ == Capacity; }
    [[nodiscard]] constexpr std::size_t staged() const noexcept { return staged_; }

    /**
     * @brief Queue `payload` for `to`, referencing the bytes in place.
     * @return `false` if the batch is already full.
     */
    [[nodiscard]] bool stage(const std::span<const std::byte> payload, const endpoint& to) noexcept { return stage(payload, to, control<Features>{}); }

    /**
     * @brief Queue `payload` for `to` with ancillary data attached.
     *
     * The control block is built per datagram and `msg_controllen` set to
     * exactly what was written, zero included. Leaving a stale length from an
     * earlier flush would have the kernel read whatever that datagram left
     * behind, which is the transmit mirror of the receive-side rearm.
     *
     * The build step gets `to.wire_family()`, not the socket's family: the
     * kernel sends to a v4-mapped peer down its IPv4 path, which ignores
     * `IPV6_TCLASS` and every other v6-level message except `IPV6_PKTINFO`.
     *
     * @return `false` if the batch is already full.
     */
    [[nodiscard]] bool stage(const std::span<const std::byte> payload, const endpoint& to, const control<Features>& ancillary) noexcept {
        if (full()) [[unlikely]] {
            return false;
        }
        const auto i{staged_++};
        addrs_[i] = to;
        // sendmmsg only reads through iov_base, but the field is not const-qualified.
        iovs_[i].iov_base = const_cast<std::byte*>(payload.data());
        iovs_[i].iov_len = payload.size();
        msgs_[i].msg_hdr.msg_namelen = to.size();
        msgs_[i].msg_hdr.msg_controllen = build_control(i, to, ancillary);
        msgs_[i].msg_len = 0;
        return true;
    }

    /**
     * @brief Queue a copy of `payload`, for bytes that will not outlive the flush.
     * @return `false` if the batch is full or the payload exceeds `SlotBytes`.
     */
    [[nodiscard]] bool stage_copy(const std::span<const std::byte> payload, const endpoint& to, const control<Features>& ancillary = {}) noexcept
        requires(SlotBytes > 0)
    {
        if (full() || payload.size() > SlotBytes) [[unlikely]] {
            return false;
        }
        const auto dest{payload_.subspan(staged_ * SlotBytes, payload.size())};
        std::ranges::copy(payload, dest.begin());
        return stage(dest, to, ancillary);
    }

    /**
     * @brief Send everything staged, then clear the batch.
     *
     * A short send is not an error: `sendmmsg` reports how many it took and the
     * remainder is dropped. Datagram delivery is unreliable by definition, so
     * requeueing the tail buys nothing a protocol layer cannot do better.
     *
     * **A failed send is different and the batch is left staged.** Nothing went
     * out, so clearing would hand the caller an empty batch and no way to know
     * what it lost, and "a protocol layer can do better" is only true while the
     * protocol still has the bytes. `EAGAIN` on a non-blocking socket is the
     * common case: flush again once it is writable, or `discard()` to drop it.
     */
    [[nodiscard]] result<std::size_t> flush(const socket& sock, const int flags = 0) noexcept {
        if (staged_ == 0) {
            return std::size_t{0};
        }
        const int n{::sendmmsg(sock.native(), msgs_.data(), static_cast<unsigned>(staged_), flags)};
        if (n < 0) [[unlikely]] {
            return fail<std::size_t>();
        }
        staged_ = 0;
        return static_cast<std::size_t>(n);
    }

    /** @brief Drop everything staged without sending it, after a failed `flush`. */
    void discard() noexcept { staged_ = 0; }

private:
    transmit_batch() noexcept = default;

    /** @brief Build slot `i`'s control block, returning the bytes `msg_controllen` must take. */
    [[nodiscard]] std::size_t build_control(const std::size_t i, const endpoint& to, const control<Features>& ancillary) noexcept {
        if constexpr (Features::control_space == 0) {
            return 0;
        } else {
            const std::span<std::byte, Features::control_space> block{control_.data() + (i * Features::control_space), Features::control_space};
            return ancillary.build_into(block, to.wire_family());
        }
    }

    void wire() noexcept {
        for (auto [i, m] : std::views::enumerate(msgs_)) {
            const auto idx{static_cast<std::size_t>(i)};
            m.msg_hdr = ::msghdr{};
            m.msg_hdr.msg_name = &addrs_[idx];
            m.msg_hdr.msg_iov = &iovs_[idx];
            m.msg_hdr.msg_iovlen = 1;
            if constexpr (Features::control_space > 0) {
                m.msg_hdr.msg_control = control_.data() + (idx * Features::control_space);
            }
        }
    }

    std::span<::mmsghdr> msgs_{};
    std::span<::iovec> iovs_{};
    std::span<endpoint> addrs_{};
    std::span<std::byte> control_{};
    std::span<std::byte> payload_{};
    std::size_t staged_{};
};

} // namespace dgram
