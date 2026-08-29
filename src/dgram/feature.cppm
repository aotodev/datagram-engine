/**
 * @file feature.cppm
 * @brief The compile-time set of ancillary-data features carried by a batch.
 *
 * A feature contributes three things that must agree: a `CMSG_SPACE` term, a
 * parse step, and a build step. Deriving the control-buffer size from the same
 * list that drives parsing is what keeps them in agreement; a hand-written size
 * that falls behind the parse list shows up as `MSG_CTRUNC` and silently missing
 * metadata.
 */
module;

#include <sys/socket.h>

export module dgram:feature;

import std;

namespace dgram {

/**
 * @brief One kind of control message the engine knows how to handle.
 *
 * `space` is the `CMSG_SPACE` contribution, alignment padding included.
 */
export template <typename T>
concept cmsg_feature = requires {
    { T::space } -> std::convertible_to<std::size_t>;
};

namespace detail {

/** @brief How many times `F` appears in `Ts`. */
template <typename F, typename... Ts>
inline constexpr std::size_t occurrences{(std::size_t{0} + ... + static_cast<std::size_t>(std::same_as<F, Ts>))};

} // namespace detail

/**
 * @brief An ordered set of `cmsg_feature`s.
 *
 * Duplicates are rejected: a repeated feature would double-count the control
 * buffer and leave its second parse step unreachable.
 */
export template <cmsg_feature... Fs>
struct features {
    static_assert(((detail::occurrences<Fs, Fs...> == 1) && ...), "a feature list must not repeat a feature");

    static constexpr std::size_t count{sizeof...(Fs)};

    /** @brief Total per-datagram control-buffer bytes. */
    static constexpr std::size_t control_space{(std::size_t{0} + ... + Fs::space)};

    /** @brief Whether `F` is a member of this set. */
    template <typename F>
    static constexpr bool contains{(std::same_as<F, Fs> || ...)};
};

/** @brief What a batch requires of its feature-set parameter. */
export template <typename T>
concept feature_set = requires {
    { T::count } -> std::convertible_to<std::size_t>;
    { T::control_space } -> std::convertible_to<std::size_t>;
};

/** @brief The empty set: no ancillary data, no control buffer. */
export using no_features = features<>;

static_assert(feature_set<no_features>);
static_assert(no_features::control_space == 0);
static_assert(no_features::count == 0);

} // namespace dgram
