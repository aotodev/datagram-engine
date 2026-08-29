/**
 * @file socket.cppm
 * @brief The UDP file descriptor and the compile-time set of options applied to it.
 */
module;

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

export module dgram:socket;

import std;
import :address;
import :error;

namespace dgram {

/**
 * @brief A setting applied once, at open time.
 *
 * Options are types rather than runtime flags so the set is fixed at the call
 * site and costs nothing to carry. `apply` runs in the order the pack is
 * written, which matters: `SO_REUSEPORT` has to precede `bind`.
 *
 * The family is passed in because the same intent needs a different option per
 * family: enabling destination-address reporting is `IP_PKTINFO` on a v4 socket
 * and `IPV6_RECVPKTINFO` on a v6 one. An option that does not care ignores it.
 */
export template <typename T>
concept socket_option = requires(int fd, family fam) {
    { T::apply(fd, fam) } -> std::same_as<result<>>;
};

namespace detail {

/** @brief `setsockopt` for any trivially copyable payload. */
template <typename T> [[nodiscard]] inline result<> set_option(const int fd, const int level, const int name, const T& value) noexcept {
    if (::setsockopt(fd, level, name, &value, static_cast<::socklen_t>(sizeof(T))) < 0) [[unlikely]] {
        return fail<>();
    }
    return {};
}

/** @brief A boolean-valued option, the shape most of `SOL_SOCKET` takes. */
template <int Level, int Name> struct flag_option {
    [[nodiscard]] static result<> apply(const int fd, family) noexcept { return set_option(fd, Level, Name, 1); }
};

} // namespace detail

/** @brief `SO_REUSEPORT`: the load-balancing primitive the engine scales on. */
export using reuse_port = detail::flag_option<SOL_SOCKET, SO_REUSEPORT>;

/** @brief `SO_REUSEADDR`. */
export using reuse_addr = detail::flag_option<SOL_SOCKET, SO_REUSEADDR>;

/** @brief `O_NONBLOCK`, so a receive returns `EAGAIN` instead of parking. */
export struct nonblocking {
    [[nodiscard]] static result<> apply(const int fd, family) noexcept {
        const int flags{::fcntl(fd, F_GETFL, 0)};
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) [[unlikely]] {
            return fail<>();
        }
        return {};
    }
};

/** @brief `SO_RCVBUF`. The kernel doubles the request and caps it at `rmem_max`. */
export template <std::size_t Bytes> struct recv_buffer {
    [[nodiscard]] static result<> apply(const int fd, family) noexcept { return detail::set_option(fd, SOL_SOCKET, SO_RCVBUF, static_cast<int>(Bytes)); }
};

/** @brief `SO_SNDBUF`. Doubled and capped like `recv_buffer`. */
export template <std::size_t Bytes> struct send_buffer {
    [[nodiscard]] static result<> apply(const int fd, family) noexcept { return detail::set_option(fd, SOL_SOCKET, SO_SNDBUF, static_cast<int>(Bytes)); }
};

/**
 * @brief `IPV6_V6ONLY`: keep a v6 socket off the v4-mapped path.
 *
 * Worth setting explicitly whenever both families are bound separately, because
 * the default is a `sysctl` and therefore not the same everywhere.
 */
export struct v6_only {
    [[nodiscard]] static result<> apply(const int fd, const family fam) noexcept {
        if (fam != family::inet6) {
            return std::unexpected{invalid_argument};
        }
        return detail::set_option(fd, IPPROTO_IPV6, IPV6_V6ONLY, 1);
    }
};

/**
 * @brief Owning UDP descriptor.
 *
 * Move-only, and closing is the destructor's job. Nothing here is thread-safe:
 * the engine's model is one socket per thread behind `SO_REUSEPORT`.
 */
export class socket {
public:
    socket() = delete;
    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    socket(socket&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

    socket& operator=(socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~socket() { close(); }

    /**
     * @brief Open a UDP socket and apply `Options` left to right.
     *
     * The descriptor is closed if any option fails, so a failed open leaks
     * nothing and the caller sees the first error rather than the last.
     */
    template <socket_option... Options> [[nodiscard]] static result<socket> open(const family fam) noexcept {
        const int domain{fam == family::inet4 ? AF_INET : AF_INET6};
        const int fd{::socket(domain, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP)};
        if (fd < 0) [[unlikely]] {
            return fail<socket>();
        }

        socket sock{fd};
        result<> applied{};
        (void)((applied = Options::apply(fd, fam), applied.has_value()) && ...);
        if (!applied) [[unlikely]] {
            return std::unexpected{applied.error()};
        }
        return sock;
    }

    [[nodiscard]] result<> bind(const endpoint& local) noexcept {
        if (::bind(fd_, local.raw(), local.size()) < 0) [[unlikely]] {
            return fail<>();
        }
        return {};
    }

    /** @brief Fix the peer, so the send path can skip per-datagram addressing. */
    [[nodiscard]] result<> connect(const endpoint& peer) noexcept {
        if (::connect(fd_, peer.raw(), peer.size()) < 0) [[unlikely]] {
            return fail<>();
        }
        return {};
    }

    /** @brief The address actually bound, which resolves an ephemeral port. */
    [[nodiscard]] result<endpoint> local_address() const noexcept {
        endpoint ep{};
        ::socklen_t len{sizeof(::sockaddr_storage)};
        if (::getsockname(fd_, reinterpret_cast<::sockaddr*>(&ep.mutable_storage()), &len) < 0) [[unlikely]] {
            return fail<endpoint>();
        }
        return ep;
    }

    [[nodiscard]] constexpr int native() const noexcept { return fd_; }

private:
    explicit constexpr socket(const int fd) noexcept : fd_{fd} {}

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_;
};

} // namespace dgram
