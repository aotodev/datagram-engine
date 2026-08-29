/**
 * @file cmsg.cppm
 * @brief Typed wrappers over the `CMSG_*` macros.
 *
 * The macros expand to C casts and pointer arithmetic that no amount of care at
 * the call site makes clean, so they are confined here and every user of
 * ancillary data goes through these. Wrapping them also gives the alignment
 * contract one place to live.
 */
module;

#include <sys/socket.h>

export module dgram:cmsg;

import std;

namespace dgram::detail {

// NOLINTBEGIN(cppcoreguidelines-pro-type-cstyle-cast, cppcoreguidelines-pro-bounds-pointer-arithmetic)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"

/**
 * @brief Bytes one control message occupies, payload plus header plus padding.
 *
 * The figure to sum when sizing a control buffer. `length_for` is the smaller
 * value that goes in `cmsg_len`, which excludes the trailing pad.
 */
export constexpr std::size_t space_for(const std::size_t payload) noexcept {
    return CMSG_SPACE(payload);
}

/** @brief The value `cmsg_len` takes for a payload of `payload` bytes. */
export constexpr std::size_t length_for(const std::size_t payload) noexcept {
    return CMSG_LEN(payload);
}

/** @brief First control message, or `nullptr` when there is no ancillary data. */
inline const ::cmsghdr* first_header(const ::msghdr& m) noexcept {
    return CMSG_FIRSTHDR(&m);
}

/** @brief Next control message, or `nullptr` at the end. Bounded by `msg_controllen`. */
inline const ::cmsghdr* next_header(const ::msghdr& m, const ::cmsghdr* current) noexcept {
    return CMSG_NXTHDR(const_cast<::msghdr*>(&m), const_cast<::cmsghdr*>(current));
}

inline ::cmsghdr* first_header(::msghdr& m) noexcept {
    return CMSG_FIRSTHDR(&m);
}

inline ::cmsghdr* next_header(::msghdr& m, ::cmsghdr* current) noexcept {
    return CMSG_NXTHDR(&m, current);
}

/** @brief Start of a control message's payload. */
inline const std::byte* data_of(const ::cmsghdr* c) noexcept {
    return reinterpret_cast<const std::byte*>(CMSG_DATA(const_cast<::cmsghdr*>(c)));
}

inline std::byte* data_of(::cmsghdr* c) noexcept {
    return reinterpret_cast<std::byte*>(CMSG_DATA(c));
}

#pragma GCC diagnostic pop
// NOLINTEND(cppcoreguidelines-pro-type-cstyle-cast, cppcoreguidelines-pro-bounds-pointer-arithmetic)

/**
 * @brief Payload bytes carried by `c`, excluding the header. Zero if malformed.
 *
 * The number to check before reading: the kernel picks the width, and it is not
 * the same across families. `IP_TOS` arrives as one byte and `IPV6_TCLASS` as a
 * four-byte `int`, so a reader that assumes either one overreads on the other.
 *
 * Saturates rather than wrapping. `CMSG_FIRSTHDR` only checks that
 * `msg_controllen` is large enough for a header, never that `cmsg_len` itself
 * is, so a buffer claiming `cmsg_len < CMSG_LEN(0)` reaches here. Subtracting in
 * `size_t` would then underflow to an enormous width, every caller's guard would
 * pass, and the read would run off the buffer.
 */
export inline std::size_t payload_size(const ::cmsghdr* c) noexcept {
    const auto len{static_cast<std::size_t>(c->cmsg_len)};
    constexpr auto header{length_for(0)};
    return len < header ? 0 : len - header;
}

/**
 * @brief Whether `c` lies wholly inside `m`'s control buffer.
 *
 * `CMSG_FIRSTHDR` only checks that `msg_controllen` can hold a header; it never
 * checks the header it then returns. A message claiming a `cmsg_len` larger than
 * the buffer that carries it therefore reaches a parser intact, and reading its
 * declared payload runs off the end.
 *
 * The kernel does not produce such a buffer. Anything that reaches this parser
 * from elsewhere might, and the walk costs one comparison per message.
 */
export inline bool within_buffer(const ::msghdr& m, const ::cmsghdr* c) noexcept {
    const auto* const base{static_cast<const std::byte*>(m.msg_control)};
    const auto* const start{reinterpret_cast<const std::byte*>(c)};
    if (base == nullptr || start < base) {
        return false;
    }
    const auto offset{static_cast<std::size_t>(start - base)};
    const auto controllen{static_cast<std::size_t>(m.msg_controllen)};
    if (offset > controllen || controllen - offset < length_for(0)) {
        return false;
    }
    const auto len{static_cast<std::size_t>(c->cmsg_len)};
    return len >= length_for(0) && len <= controllen - offset;
}

/** @brief The payload as bytes, sized by `cmsg_len`. */
inline std::span<const std::byte> payload_of(const ::cmsghdr* c) noexcept {
    return {data_of(c), payload_size(c)};
}

/**
 * @brief Copy a trivially copyable payload out of a control message.
 *
 * @pre `payload_size(c) >= sizeof(T)`, which the caller establishes by checking
 *      the width the kernel actually used.
 */
export template <typename T>
    requires std::is_trivially_copyable_v<T>
T read_payload(const ::cmsghdr* c) noexcept pre(payload_size(c) >= sizeof(T)) {
    T value{};
    std::memcpy(&value, data_of(c), sizeof(T));
    return value;
}

/**
 * @brief Write one control message into `dst` and return the bytes it occupies.
 *
 * @pre `dst` is aligned for a `cmsghdr` and has room for `space_for(sizeof(T))`.
 */
export template <typename T>
    requires std::is_trivially_copyable_v<T>
std::size_t write_message(::cmsghdr* dst, const int level, const int type, const T& value) noexcept {
    dst->cmsg_level = level;
    dst->cmsg_type = type;
    dst->cmsg_len = length_for(sizeof(T));
    std::memcpy(data_of(dst), &value, sizeof(T));
    return space_for(sizeof(T));
}

} // namespace dgram::detail
