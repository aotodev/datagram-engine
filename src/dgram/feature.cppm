// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file feature.cppm
 * @brief The compile-time set of ancillary-data features carried by a batch.
 *
 * A feature contributes four things that must agree: a `CMSG_SPACE` term, the
 * `(level, type)` pair it answers to, a parse step, and a build step. Deriving
 * the control-buffer size from the same list that drives parsing is what keeps
 * them in agreement; a hand-written size that falls behind the parse list shows
 * up as `MSG_CTRUNC` and silently missing metadata.
 */
module;

#include <sys/socket.h>

export module dgram:feature;

import std;

import :address;
import :cmsg;
import :error;

namespace dgram {

/**
 * @brief Anything that can occupy space in a control buffer.
 *
 * `space` is the `CMSG_SPACE` contribution, alignment padding included, and for
 * a feature that appears in both families it is the larger of the two: the
 * receive path cannot know which will arrive.
 *
 * Deliberately the bare minimum. Not every feature travels in both directions:
 * `UDP_GRO` is only ever received and `UDP_SEGMENT` only ever sent, so requiring
 * the full set here would force one of them to carry a member that is never
 * called. The direction-specific refinements below say which is which, and a
 * receive and a transmit batch take their own feature sets.
 */
export template <typename T>
concept cmsg_feature = requires {
    typename T::value_type;
    { T::space } -> std::convertible_to<std::size_t>;
};

/** @brief A `cmsg_feature` that can be recognised and read off a received datagram. */
export template <typename T>
concept parseable_feature = cmsg_feature<T> && requires(const ::cmsghdr* c, int level, int type) {
    { T::matches(level, type) } -> std::same_as<bool>;
    { T::parse(c) } -> std::same_as<std::optional<typename T::value_type>>;
};

/** @brief A `cmsg_feature` that can be attached to an outgoing datagram. */
export template <typename T>
concept sendable_feature = cmsg_feature<T> && requires(::cmsghdr* dst, const typename T::value_type& v, family fam) {
    { T::build(dst, v, fam) } -> std::same_as<std::size_t>;
};

/** @brief A `parseable_feature` the kernel must be told to report. */
export template <typename T>
concept receivable_feature = parseable_feature<T> && requires(int fd, family fam) {
    { T::enable(fd, fam) } -> std::same_as<result<>>;
};

namespace detail {

/** @brief How many times `F` appears in `Ts`. */
template <typename F, typename... Ts> inline constexpr std::size_t occurrences{(std::size_t{0} + ... + static_cast<std::size_t>(std::same_as<F, Ts>))};

/** @brief Position of `F` in `Ts`, for indexing the value tuple. */
template <typename F, typename... Ts> consteval std::size_t index_of() noexcept {
    std::size_t index{};
    bool found{false};
    (void)((std::same_as<F, Ts> ? (found = true) : (index += !found, false)) || ...);
    return index;
}

} // namespace detail

/**
 * @brief An ordered set of `cmsg_feature`s.
 *
 * Duplicates are rejected: a repeated feature would double-count the control
 * buffer and leave its second parse step unreachable.
 */
export template <cmsg_feature... Fs> struct features {
    static_assert(((detail::occurrences<Fs, Fs...> == 1) && ...), "a feature list must not repeat a feature");

    static constexpr std::size_t count{sizeof...(Fs)};

    /** @brief Total per-datagram control-buffer bytes. */
    static constexpr std::size_t control_space{(std::size_t{0} + ... + Fs::space)};

    /** @brief Whether `F` is a member of this set. */
    template <typename F> static constexpr bool contains{(std::same_as<F, Fs> || ...)};

    /** @brief Position of `F`, for indexing `values`. */
    template <typename F>
        requires contains<F>
    static constexpr std::size_t index{detail::index_of<F, Fs...>()};

    /** @brief Per-feature slots, each absent until a matching control message is parsed. */
    using values = std::tuple<std::optional<typename Fs::value_type>...>;

    /** @brief Type tags, for expanding over the set with `template for`. */
    using tags = std::tuple<std::type_identity<Fs>...>;
};

/** @brief What a batch requires of its feature-set parameter. */
export template <typename T>
concept feature_set = requires {
    { T::count } -> std::convertible_to<std::size_t>;
    { T::control_space } -> std::convertible_to<std::size_t>;
    typename T::values;
    typename T::tags;
};

/** @brief The empty set: no ancillary data, no control buffer. */
export using no_features = features<>;

static_assert(feature_set<no_features>);
static_assert(no_features::control_space == 0);
static_assert(no_features::count == 0);

/* ============================================================================
 * Parsed metadata
 * ============================================================================ */

/**
 * @brief What the enabled features found on one received datagram.
 *
 * Every slot is independently absent: the kernel sends what it has, and an
 * option that was never enabled, or a datagram that carried no such header,
 * simply yields nothing rather than a zeroed value.
 */
export template <feature_set Features> class metadata {
public:
    /** @brief `F`'s value, or nothing if no matching control message arrived. */
    template <cmsg_feature F>
        requires Features::template
    contains<F> [[nodiscard]] constexpr const std::optional<typename F::value_type>& get() const& noexcept {
        return std::get<Features::template index<F>>(values_);
    }

    /**
     * @brief Same, by value.
     *
     * `meta()` yields a temporary, so `d.meta().get<F>()` would otherwise hand
     * back a reference into an object that dies at the end of the expression.
     */
    template <cmsg_feature F>
        requires Features::template
    contains<F> [[nodiscard]] constexpr std::optional<typename F::value_type> get() const&& noexcept {
        return std::get<Features::template index<F>>(values_);
    }

    template <cmsg_feature F>
        requires Features::template
    contains<F> constexpr void set(std::optional<typename F::value_type> value) noexcept {
        std::get<Features::template index<F>>(values_) = std::move(value);
    }

private:
    typename Features::values values_{};
};

/** @brief Nothing to parse and nothing to hold. Not exported: a specialization never is. */
template <> class metadata<no_features> {};

namespace detail {

/**
 * @brief Walk one datagram's control buffer and hand each message to its feature.
 *
 * Bounded by `msg_controllen`, which after `recvmmsg` holds what was actually
 * received. A truncated buffer therefore yields a short parse rather than a read
 * past the end, and the loss is reported separately as `MSG_CTRUNC`.
 *
 * Each message is also checked against the buffer before a feature sees it: a
 * `cmsg_len` that overruns what was received passes `CMSG_FIRSTHDR` untouched.
 */
export template <feature_set Features> [[nodiscard]] metadata<Features> parse_control(const ::msghdr& m) noexcept {
    metadata<Features> out{};
    if constexpr (Features::count > 0) {
        static constexpr typename Features::tags tags{};
        for (const ::cmsghdr* c{first_header(m)}; c != nullptr; c = next_header(m, c)) {
            // A message whose declared length overruns the buffer cannot be
            // trusted, and neither can anything after it: stop rather than skip.
            if (!within_buffer(m, c)) {
                break;
            }
            template for (constexpr auto tag : tags) {
                using feature = typename decltype(tag)::type;
                if constexpr (parseable_feature<feature>) {
                    if (feature::matches(c->cmsg_level, c->cmsg_type)) {
                        out.template set<feature>(feature::parse(c));
                    }
                }
            }
        }
    }
    return out;
}

} // namespace detail

/* ============================================================================
 * Outgoing metadata
 * ============================================================================ */

/**
 * @brief Ancillary data to attach to an outgoing datagram.
 *
 * The transmit mirror of `metadata`. Unset features contribute nothing, so a
 * control block is only as large as what was actually asked for.
 */
export template <feature_set Features> class control {
public:
    template <sendable_feature F>
        requires Features::template
    contains<F> constexpr control& set(typename F::value_type value) noexcept {
        std::get<Features::template index<F>>(values_) = std::move(value);
        return *this;
    }

    template <cmsg_feature F>
        requires Features::template
    contains<F> [[nodiscard]] constexpr const std::optional<typename F::value_type>& get() const& noexcept {
        return std::get<Features::template index<F>>(values_);
    }

    template <cmsg_feature F>
        requires Features::template
    contains<F> [[nodiscard]] constexpr std::optional<typename F::value_type> get() const&& noexcept {
        return std::get<Features::template index<F>>(values_);
    }

    /**
     * @brief Write every set feature into `buffer`, returning the bytes used.
     *
     * The return value is what `msg_controllen` must become for this datagram,
     * and it is zero when nothing was set. Passing a stale length instead makes
     * the kernel read whatever the previous datagram left behind.
     *
     * The buffer's extent is static rather than checked: a contract here would
     * be compiled out, because this template instantiates in the caller's
     * translation unit, which need not have been built with `-fcontracts`. A
     * precondition that is silently inert is worse than a type that cannot be
     * given the wrong size.
     *
     * @pre `buffer` is aligned for a `cmsghdr`.
     */
    [[nodiscard]] std::size_t build_into(const std::span<std::byte, Features::control_space> buffer, const family fam) const noexcept {
        std::size_t used{};
        if constexpr (Features::count > 0) {
            static constexpr typename Features::tags tags{};
            template for (constexpr auto tag : tags) {
                using feature = typename decltype(tag)::type;
                if constexpr (sendable_feature<feature>) {
                    if (const auto& value{get<feature>()}; value.has_value()) {
                        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): cmsghdr-aligned by construction
                        auto* dst{reinterpret_cast<::cmsghdr*>(buffer.data() + used)};
                        used += feature::build(dst, *value, fam);
                    }
                }
            }
        }
        return used;
    }

private:
    typename Features::values values_{};
};

/** @brief Nothing to build. Not exported: a specialization never is. */
template <> class control<no_features> {
public:
    [[nodiscard]] static constexpr std::size_t build_into(std::span<std::byte, 0>, family) noexcept { return 0; }
};

} // namespace dgram
