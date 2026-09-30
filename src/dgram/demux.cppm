// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file demux.cppm
 * @brief Routing arrived datagrams to the protocol state that owns them.
 *
 * The boundary the whole engine exists to reach: above this line a protocol sees
 * bytes, an address and a time, and nothing about sockets, `msghdr` or file
 * descriptors.
 *
 * There is no locking and no atomic here. The engine's model is one socket, one
 * arena and one table per thread behind `SO_REUSEPORT`, so a flow table is never
 * shared and a lock-free map would be paying contention costs for sharing that
 * does not happen.
 */
module;

#include <cassert>
#include <cerrno>

export module dgram:demux;

import std;
import libmem;

import :address;
import :error;
import :feature;
import :hash;
import :metadata;
import :offload;
import :pacing;

namespace dgram {

/* ============================================================================
 * What a protocol sees
 * ============================================================================ */

/**
 * @brief One datagram, presented without any reference to how it was received.
 *
 * A view, not a value: it holds references and cannot be reseated or assigned.
 * The three borrows do not all last as long, so none of them may be kept:
 *
 * | member    | valid until                                  |
 * |-----------|----------------------------------------------|
 * | `payload` | the batch's next `receive`                    |
 * | `from`    | the batch's next `receive`                    |
 * | `meta`    | **the end of the `on_datagram` call**         |
 *
 * `meta` is the short one because `route` parses it into a local, once per slot
 * rather than once per datagram. A protocol that keeps anything copies it, and
 * for `meta` that means copying inside the call.
 */
export template <feature_set Features> struct arrival {
    std::span<const std::byte> payload; ///< one datagram, already de-coalesced
    const endpoint& from;               ///< peer address
    const metadata<Features>& meta;     ///< parsed ancillary data, shared across a coalesced slot
    departure at;                       ///< when the loop observed it
};

/**
 * @brief Protocol state that can accept a datagram.
 *
 * The only thing the engine requires of a protocol.
 */
export template <typename T, typename Features>
concept datagram_sink = requires(T& sink, const arrival<Features>& a) { sink.on_datagram(a); };

/* ============================================================================
 * Keys
 * ============================================================================ */

/**
 * @brief A protocol identifier carried in the payload, such as a QUIC connection id.
 *
 * `MaxBytes` bounds it inline, so a key never allocates and a table of them is
 * one contiguous block.
 */
export template <std::size_t MaxBytes> class byte_key {
public:
    constexpr byte_key() noexcept = default;

    explicit constexpr byte_key(const std::span<const std::byte> bytes) noexcept {
        const auto n{std::min(bytes.size(), MaxBytes)};
        for (std::size_t i{}; i < n; ++i) {
            bytes_.push_back(bytes[i]);
        }
    }

    [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept { return {bytes_.data(), bytes_.size()}; }
    [[nodiscard]] constexpr bool empty() const noexcept { return bytes_.empty(); }

    [[nodiscard]] friend constexpr bool operator==(const byte_key& a, const byte_key& b) noexcept { return std::ranges::equal(a.bytes_, b.bytes_); }

    /** @brief Length first, so a key composed of this and more fields stays prefix-free. */
    template <byte_hasher H> friend constexpr void hash_append(H& h, const byte_key& k) noexcept {
        hash_append(h, static_cast<length_type>(k.bytes_.size()));
        h(k.bytes());
    }

private:
    using length_type = std::conditional_t<(MaxBytes <= std::numeric_limits<std::uint8_t>::max()), std::uint8_t, std::size_t>;

    std::inplace_vector<std::byte, MaxBytes> bytes_{};
};

/** @brief Peer address and port. Enough when the socket is bound to one local address. */
export struct peer_key {
    endpoint remote{};

    [[nodiscard]] friend bool operator==(const peer_key& a, const peer_key& b) noexcept { return a.remote == b.remote; }
    template <byte_hasher H> friend void hash_append(H& h, const peer_key& k) noexcept { hash_append(h, k.remote); }
};

/**
 * @brief The full 4-tuple: peer and the local address the datagram arrived on.
 *
 * What a wildcard-bound socket needs, since two peers can reach it on different
 * local addresses. The local half comes from `pktinfo`, so that feature has to
 * be in the receive set or every key carries an unspecified local address.
 */
export struct flow_key {
    endpoint remote{};
    endpoint local{};

    [[nodiscard]] friend bool operator==(const flow_key& a, const flow_key& b) noexcept { return a.remote == b.remote && a.local == b.local; }

    template <byte_hasher H> friend void hash_append(H& h, const flow_key& k) noexcept {
        hash_append(h, k.remote);
        hash_append(h, k.local);
    }
};

/**
 * @brief A key usable by `flow_table`: equality-comparable, and feeds its identity to a hasher.
 *
 * `hash_append` must feed exactly the fields `operator==` compares, in a
 * prefix-free encoding: equal keys must hash equal, and two unequal keys that
 * feed the same bytes collide under every seed.
 */
export template <typename K>
concept demux_key = std::equality_comparable<K> && std::is_trivially_destructible_v<K> && requires(siphash& h, const K& k) { hash_append(h, k); };

static_assert(demux_key<peer_key>);
static_assert(demux_key<flow_key>);
static_assert(demux_key<byte_key<20>>);

/* ============================================================================
 * Projections
 * ============================================================================ */

/** @brief Turns an arrival into the key of the flow that owns it, or nothing. */
export template <typename P, typename Features>
concept key_projection = requires(const P& p, const arrival<Features>& a) {
    { p(a) } -> std::convertible_to<std::optional<typename P::key_type>>;
};

/** @brief Key on the peer alone. */
export struct by_peer {
    using key_type = peer_key;

    template <feature_set Features> [[nodiscard]] std::optional<key_type> operator()(const arrival<Features>& a) const noexcept { return key_type{a.from}; }
};

/**
 * @brief Key on the full 4-tuple.
 *
 * Yields nothing when the local address is unknown, which means `pktinfo` was
 * not in the receive set. Silently keying on an unspecified local address would
 * collapse every local address into one flow.
 */
export struct by_flow {
    using key_type = flow_key;

    template <feature_set Features>
    [[nodiscard]] std::optional<key_type> operator()(const arrival<Features>& a) const noexcept
        requires(Features::template contains<pktinfo>)
    {
        const auto& local{a.meta.template get<pktinfo>()};
        if (!local.has_value()) {
            return std::nullopt;
        }
        return key_type{a.from, local->address};
    }
};

/**
 * @brief Key on bytes at a fixed offset in the payload, as QUIC's connection id is.
 *
 * This is why the key is a projection rather than something the engine picks: a
 * QUIC connection survives its peer changing address, so the 4-tuple is exactly
 * the wrong key for it.
 *
 * Yields nothing for a datagram too short to contain the field, rather than
 * reading past the end.
 */
export template <std::size_t Offset, std::size_t Length> struct by_payload_id {
    static_assert(Length > 0, "a zero-length identifier cannot distinguish anything");

    using key_type = byte_key<Length>;

    template <feature_set Features> [[nodiscard]] std::optional<key_type> operator()(const arrival<Features>& a) const noexcept {
        if (a.payload.size() < Offset + Length) {
            return std::nullopt;
        }
        return key_type{a.payload.subspan(Offset, Length)};
    }
};

/* ============================================================================
 * The table
 * ============================================================================ */

/** @brief What routing needs of a table; substitute your own if this one does not fit. */
export template <typename T, typename Key, typename Value>
concept flow_lookup = requires(T& t, const T& ct, const Key& k, Value v) {
    { ct.find(k) } -> std::convertible_to<const Value*>;
    { t.find(k) } -> std::same_as<Value*>;
    { t.insert(k, std::move(v)) } -> std::same_as<Value*>;
    { t.erase(k) } -> std::same_as<bool>;
    { ct.size() } -> std::convertible_to<std::size_t>;
};

/**
 * @brief Open-addressing flow table carved from a resource.
 *
 * Linear probing with backward-shift deletion, so there are no tombstones to
 * accumulate and a long-lived table does not degrade as flows come and go.
 * `Slots` must be a power of two; the table refuses to fill past three
 * quarters of it, because a nearly full open-addressed table degenerates into a
 * linear scan and this one cannot grow.
 *
 * Buckets come from SipHash under the table's own seed, so keys a peer picks to
 * collide in one table spread out in another.
 *
 * Not thread-safe. One per thread, like everything else.
 */
export template <demux_key Key, typename Value, std::size_t Slots> class flow_table {
    static_assert(Slots >= 8 && (Slots & (Slots - 1)) == 0, "Slots must be a power of two, at least 8");
    static_assert(std::is_trivially_destructible_v<Value>, "the table never runs a destructor; store a handle or an index");

public:
    using key_type = Key;
    using value_type = Value;

    /** @brief Flows the table will hold before refusing to grow. */
    static constexpr std::size_t max_size{Slots / 4 * 3};

    static constexpr std::size_t footprint() noexcept {
        return Slots * sizeof(Key) + alignof(Key) - 1 + Slots * sizeof(Value) + alignof(Value) - 1 + Slots + alignof(std::byte) - 1;
    }

    /** @brief Carve the table, hashing under `seed`. A seed a peer can learn or guess makes it floodable. */
    template <libmem::aligned_monotonic_resource R> [[nodiscard]] static result<flow_table> carve(R& resource, const hash_seed seed) noexcept {
        flow_table t{};
        t.seed_ = seed;
        auto* keys{static_cast<Key*>(resource.allocate(Slots * sizeof(Key), alignof(Key)))};
        auto* values{static_cast<Value*>(resource.allocate(Slots * sizeof(Value), alignof(Value)))};
        auto* used{static_cast<std::uint8_t*>(resource.allocate(Slots, alignof(std::uint8_t)))};
        if (keys == nullptr || values == nullptr || used == nullptr) [[unlikely]] {
            return std::unexpected{out_of_memory};
        }
        t.keys_ = {keys, Slots};
        t.values_ = {values, Slots};
        t.used_ = {used, Slots};
        std::ranges::uninitialized_value_construct(t.keys_);
        std::ranges::uninitialized_value_construct(t.values_);
        std::ranges::fill(t.used_, std::uint8_t{0});
        return t;
    }

    /** @brief Carve the table under a seed from `random_seed()`. */
    template <libmem::aligned_monotonic_resource R> [[nodiscard]] static result<flow_table> carve(R& resource) noexcept {
        return random_seed() | then([&resource](const hash_seed seed) { return carve(resource, seed); });
    }

    [[nodiscard]] Value* find(const Key& key) noexcept {
        const auto slot{locate(key)};
        return slot ? &values_[*slot] : nullptr;
    }

    [[nodiscard]] const Value* find(const Key& key) const noexcept {
        const auto slot{locate(key)};
        return slot ? &values_[*slot] : nullptr;
    }

    /**
     * @brief Insert or overwrite.
     * @return The stored value, or `nullptr` when the table is at `max_size`.
     */
    [[nodiscard]] Value* insert(const Key& key, Value value) noexcept {
        auto index{bucket(key)};
        while (used_[index] != 0) {
            if (keys_[index] == key) {
                values_[index] = std::move(value);
                return &values_[index];
            }
            index = next(index);
        }
        if (size_ >= max_size) [[unlikely]] {
            return nullptr;
        }
        keys_[index] = key;
        values_[index] = std::move(value);
        used_[index] = 1;
        ++size_;
        return &values_[index];
    }

    /**
     * @brief Remove `key`, closing the probe chain behind it.
     *
     * Not `[[nodiscard]]`: erasing a flow you know is there is the ordinary
     * case, and the return exists for the caller who does not know.
     */
    bool erase(const Key& key) noexcept {
        const auto found{locate(key)};
        if (!found) {
            return false;
        }
        auto hole{*found};
        used_[hole] = 0;
        --size_;

        // Backward shift: pull each following entry back if the hole is still
        // on its probe path. Without this, linear probing needs tombstones and
        // a table that churns fills with them.
        auto scan{next(hole)};
        while (used_[scan] != 0) {
            const auto home{bucket(keys_[scan])};
            const auto shift_ok{(scan >= hole) ? (home <= hole || home > scan) : (home <= hole && home > scan)};
            if (shift_ok) {
                keys_[hole] = keys_[scan];
                values_[hole] = std::move(values_[scan]);
                used_[hole] = 1;
                used_[scan] = 0;
                hole = scan;
            }
            scan = next(scan);
        }
        return true;
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ >= max_size; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return max_size; }

    void clear() noexcept {
        std::ranges::fill(used_, std::uint8_t{0});
        size_ = 0;
    }

    /** @brief Lazy view of every live (key, value) pair, in slot order. */
    [[nodiscard]] auto entries() const noexcept {
        return std::views::iota(std::size_t{0}, Slots) | std::views::filter([this](const std::size_t i) { return used_[i] != 0; }) |
               std::views::transform([this](const std::size_t i) { return std::pair<const Key&, const Value&>{keys_[i], values_[i]}; });
    }

private:
    flow_table() noexcept = default;

    [[nodiscard]] std::size_t bucket(const Key& key) const noexcept {
        siphash h{seed_};
        hash_append(h, key);
        return static_cast<std::size_t>(h.finish()) & (Slots - 1);
    }
    [[nodiscard]] static constexpr std::size_t next(const std::size_t index) noexcept { return (index + 1) & (Slots - 1); }

    [[nodiscard]] std::optional<std::size_t> locate(const Key& key) const noexcept {
        auto index{bucket(key)};
        for (std::size_t probes{}; probes < Slots; ++probes) {
            if (used_[index] == 0) {
                return std::nullopt;
            }
            if (keys_[index] == key) {
                return index;
            }
            index = next(index);
        }
        return std::nullopt;
    }

    std::span<Key> keys_{};
    std::span<Value> values_{};
    std::span<std::uint8_t> used_{};
    std::size_t size_{};
    hash_seed seed_{};
};

/* ============================================================================
 * Routing
 * ============================================================================ */

namespace detail {

/**
 * @brief The sink behind a table value.
 *
 * A `flow_table` stores trivially destructible values, so protocol state is
 * normally held as a pointer or an index into a pool rather than inline. Both
 * shapes route the same way.
 */
template <typename V> [[nodiscard]] constexpr decltype(auto) sink_of(V& value) noexcept {
    if constexpr (std::is_pointer_v<V>) {
        return *value;
    } else {
        return (value);
    }
}

/**
 * @brief Hand an arrival to a sink, with whatever `route` was given to pass on.
 *
 * A sink that does not want the context keeps the one-argument form, so adding
 * context to a `route` call cannot break a sink that ignores it.
 */
template <typename Sink, typename Features, typename... Context> constexpr void deliver(Sink&& sink, const arrival<Features>& a, Context&&... context) {
    if constexpr (requires { sink.on_datagram(a, context...); }) {
        sink.on_datagram(a, std::forward<Context>(context)...);
    } else {
        static_assert(datagram_sink<std::remove_reference_t<Sink>, Features>, "protocol state behind a flow table value must have on_datagram(const arrival&)");
        sink.on_datagram(a);
    }
}

/**
 * @brief Run the unmatched callback, reporting whether it took the datagram.
 *
 * A callback returning `void` never claims one, which is what the discarding
 * overload of `route` wants and what every caller written before this did.
 */
template <typename Unmatched, typename Features, typename Key>
[[nodiscard]] constexpr bool claimed(Unmatched&& unmatched, const arrival<Features>& a, const std::optional<Key>& key) {
    if constexpr (std::same_as<std::invoke_result_t<Unmatched&, const arrival<Features>&, const std::optional<Key>&>, void>) {
        std::invoke(unmatched, a, key);
        return false;
    } else {
        return static_cast<bool>(std::invoke(unmatched, a, key));
    }
}

} // namespace detail

/** @brief What `route` did with one batch. */
export struct routed {
    std::size_t slots{};     ///< receive slots walked, coalesced or not
    std::size_t delivered{}; ///< datagrams handed to an existing flow
    std::size_t accepted{};  ///< datagrams the `unmatched` callback claimed, by returning `true`
    std::size_t unmatched{}; ///< datagrams with no flow, or no key at all
    std::size_t dropped{};   ///< datagrams that arrived truncated

    /** @brief Datagrams that reached a protocol, however they got there. */
    [[nodiscard]] constexpr std::size_t handled() const noexcept { return delivered + accepted; }

    /**
     * @brief Datagrams per slot, which is what `gro` is buying.
     *
     * One means the kernel coalesced nothing, and a receive slot sized for a
     * coalesced buffer is being paid for and not used.
     *
     * A truncated slot counts in `slots` and contributes no datagrams, so a
     * batch that arrived entirely truncated reads as zero rather than one: an
     * undersized slot is the other way this ratio goes wrong, and it should not
     * look like a slot that was used.
     */
    [[nodiscard]] constexpr double per_slot() const noexcept {
        const auto seen{handled() + unmatched};
        return slots == 0 ? 0.0 : static_cast<double>(seen) / static_cast<double>(slots);
    }
};

/**
 * @brief Route every datagram in `batch` to the flow that owns it.
 *
 * Routing is per datagram, not per slot: with GRO a single slot can hold
 * datagrams belonging to different flows, and dispatching on the slot would
 * hand all of them to whichever flow the first one keyed to.
 *
 * The metadata is parsed once per slot and shared by its segments, since
 * ancillary data describes the slot rather than the datagrams inside it.
 *
 * @param unmatched Called with the arrival when no key or no flow was found;
 *                  where a server decides whether to accept a new connection.
 */
export template <typename Batch, typename Projection, typename Table, typename Unmatched, typename... Context>
    requires key_projection<Projection, typename Batch::features_type>
[[nodiscard]] routed route(const Batch& batch, const Projection& key_of, Table& table, const departure at, Unmatched&& unmatched, Context&&... context) {
    using features_type = typename Batch::features_type;

    routed counts{};
    const auto slots{batch.datagrams()};
    for (const auto& slot : slots) {
        ++counts.slots;
        if (!slot.intact()) {
            ++counts.dropped;
            continue;
        }
        const auto meta{slot.meta()};
        std::size_t stride{};
        if constexpr (features_type::template contains<gro>) {
            if (const auto size{meta.template get<gro>()}) {
                stride = *size;
            }
        }

        for (const auto& payload : segments_of(slot.payload(), stride)) {
            const arrival<features_type> a{payload, slot.from(), meta, at};
            const auto key{key_of(a)};
            auto* flow{key ? table.find(*key) : nullptr};
            if (flow != nullptr) {
                detail::deliver(detail::sink_of(*flow), a, context...);
                ++counts.delivered;
            } else if (detail::claimed(unmatched, a, key)) {
                ++counts.accepted;
            } else {
                ++counts.unmatched;
            }
        }
    }
    return counts;
}

/** @brief Route, discarding anything that does not match a known flow. */
export template <typename Batch, typename Projection, typename Table>
    requires key_projection<Projection, typename Batch::features_type>
[[nodiscard]] routed route(const Batch& batch, const Projection& key_of, Table& table, const departure at) {
    return route(batch, key_of, table, at, [](const auto&, const auto&) static noexcept { return false; });
}

} // namespace dgram
