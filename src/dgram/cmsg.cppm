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
 * @brief Payload bytes carried by `c`, excluding the header.
 *
 * The number to check before reading: the kernel picks the width, and it is not
 * the same across families. `IP_TOS` arrives as one byte and `IPV6_TCLASS` as a
 * four-byte `int`, so a reader that assumes either one overreads on the other.
 */
inline std::size_t payload_size(const ::cmsghdr* c) noexcept {
    return static_cast<std::size_t>(c->cmsg_len) - length_for(0);
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
