/**
 * @file error.cppm
 * @brief Syscall error monad and the pipeable combinators over it.
 */
module;

#include <cerrno>
#include <cstring>

export module dgram:error;

import std;

namespace dgram {

/**
 * @brief A POSIX `errno` value, never zero.
 *
 * Zero is not representable: a `result` holding an error always holds a real
 * failure, so `has_value()` is the only success test needed.
 */
export enum class errc : int {};

/**
 * @brief The `errno` values a caller routinely branches on.
 *
 * Exported so consuming code never needs `<cerrno>`: macros do not cross a
 * module boundary, so importing `dgram` would otherwise not be enough.
 */
export inline constexpr errc interrupted{EINTR};
export inline constexpr errc would_block{EAGAIN};
export inline constexpr errc out_of_memory{ENOMEM};
export inline constexpr errc invalid_argument{EINVAL};
export inline constexpr errc message_too_long{EMSGSIZE};
export inline constexpr errc connection_refused{ECONNREFUSED};
export inline constexpr errc address_in_use{EADDRINUSE};

/** @brief Human-readable text for an `errc`, from `strerrordesc_np`. */
export [[nodiscard]] inline std::string_view describe(const errc e) noexcept {
    const char* text{::strerrordesc_np(std::to_underlying(e))};
    return text ? std::string_view{text} : std::string_view{"unknown error"};
}

/** @brief The result of anything that can fail at the syscall boundary. */
export template <typename T = void>
using result = std::expected<T, errc>;

/** @brief Build a failed `result<T>` from the current `errno`. */
export template <typename T = void>
[[nodiscard]] inline result<T> fail() noexcept {
    return std::unexpected{errc{errno}};
}

/**
 * @brief Run a syscall that reports failure as a negative return.
 *
 * `EINTR` is not retried here. A caller that parks on readiness wants to see
 * it, and one that does not can retry at its own layer.
 */
export template <typename F, typename... Args>
    requires std::invocable<F, Args...> && std::signed_integral<std::invoke_result_t<F, Args...>>
[[nodiscard]] auto invoke_syscall(F&& f, Args&&... args) noexcept -> result<std::invoke_result_t<F, Args...>> {
    const auto rc{std::invoke(std::forward<F>(f), std::forward<Args>(args)...)};
    if (rc < 0) [[unlikely]] {
        return fail<decltype(rc)>();
    }
    return rc;
}

/* ============================================================================
 * Pipeable combinators
 *
 * `std::expected` carries `and_then` / `transform` / `or_else` as members, which
 * do not compose left to right. These wrap them so a chain of fallible steps
 * reads in the order it runs.
 * ============================================================================ */

namespace detail {

template <typename Fn, typename Tag>
struct closure {
    Fn fn;

    template <typename E>
    friend constexpr auto operator|(E&& e, closure c) {
        return Tag::apply(std::forward<E>(e), std::move(c.fn));
    }
};

struct then_tag {
    static constexpr auto apply(auto&& e, auto&& f) { return std::forward<decltype(e)>(e).and_then(std::forward<decltype(f)>(f)); }
};

struct map_tag {
    static constexpr auto apply(auto&& e, auto&& f) { return std::forward<decltype(e)>(e).transform(std::forward<decltype(f)>(f)); }
};

struct recover_tag {
    static constexpr auto apply(auto&& e, auto&& f) { return std::forward<decltype(e)>(e).or_else(std::forward<decltype(f)>(f)); }
};

} // namespace detail

/** @brief Chain a step that itself returns a `result`. */
export template <typename Fn>
[[nodiscard]] constexpr auto then(Fn&& fn) {
    return detail::closure<std::decay_t<Fn>, detail::then_tag>{std::forward<Fn>(fn)};
}

/** @brief Transform the value, leaving an error untouched. */
export template <typename Fn>
[[nodiscard]] constexpr auto map(Fn&& fn) {
    return detail::closure<std::decay_t<Fn>, detail::map_tag>{std::forward<Fn>(fn)};
}

/** @brief Handle an error, producing a `result` of the same value type. */
export template <typename Fn>
[[nodiscard]] constexpr auto recover(Fn&& fn) {
    return detail::closure<std::decay_t<Fn>, detail::recover_tag>{std::forward<Fn>(fn)};
}

/**
 * @brief Run `fn` for its effect and pass the value through unchanged.
 *
 * The escape hatch for logging or counting inside a chain, so a side effect does
 * not have to break the pipe apart. Yields by value: `expected` cannot hold a
 * reference, so forwarding one out of here would not compile.
 */
export template <typename Fn>
[[nodiscard]] constexpr auto tap(Fn&& fn) {
    return map([f = std::forward<Fn>(fn)]<typename V>(V&& v) -> std::remove_cvref_t<V> {
        std::invoke(f, std::as_const(v));
        return std::forward<V>(v);
    });
}

} // namespace dgram
