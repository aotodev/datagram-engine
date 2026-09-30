# Sockets and addresses

## `endpoint`

An IPv4 or IPv6 address and port, held in the kernel's `sockaddr_storage`
layout. Trivially destructible and cheap to copy.

| Member | Description |
|--------|-------------|
| `endpoint()` | Unspecified family. `valid()` is `false`. |
| `endpoint(const sockaddr_storage&)` | Adopt a kernel-filled address. |
| `static result<endpoint> parse(family, const char* text, uint16_t port)` | Parse `192.0.2.1` or `2001:db8::1`. `invalid_argument` if `text` does not parse for `family`. |
| `static endpoint any(family, uint16_t port)` | The wildcard address, for binding. |
| `bool is_v4() / is_v6() / valid()` | Family predicates. |
| `bool is_v4_mapped()` | A v6 address of the form `::ffff:a.b.c.d`, which the kernel sends and receives as IPv4. |
| `family wire_family()` | The family a datagram to this endpoint travels as: `inet4` for a v4-mapped address. |
| `uint16_t port()` | Port in host byte order. |
| `socklen_t size()` | Bytes the kernel needs for this family, not `sizeof(sockaddr_storage)`. |
| `const sockaddr* raw()` | Pointer for passing to a syscall. |
| `std::string text()` | `addr:port`, or `[addr]:port` for IPv6. Allocates. |
| `operator==` | Compares family, then the significant prefix only. |

## `socket`

An owning UDP descriptor. Move-only; the destructor closes it. Nothing on it is
thread-safe, by design: the engine's model is one socket per thread behind
`SO_REUSEPORT`.

```cpp
template <socket_option... Options>
static result<socket> open(family fam);
```

Creates a `SOCK_DGRAM` socket with `SOCK_CLOEXEC` and applies `Options` left to
right. Returns the first error and closes the descriptor, so a failed open leaks
nothing.

| Member | Description |
|--------|-------------|
| `result<> bind(const endpoint& local)` | Bind to a local address. |
| `result<> connect(const endpoint& peer)` | Fix the peer, so the send path can skip per-datagram addressing. |
| `result<endpoint> local_address()` | The address actually bound, which resolves an ephemeral port. |
| `int native()` | The underlying descriptor. |

## Options

An option is a type with `static result<> apply(int fd, family)`, described by
the `socket_option` concept. They are applied in the order written, which
matters: `SO_REUSEPORT` has to precede `bind`.

The family is passed in because the same intent needs a different option per
family: enabling destination-address reporting is `IP_PKTINFO` on a v4 socket and
`IPV6_RECVPKTINFO` on a v6 one. An option that does not care ignores it.

| Option | Effect |
|--------|--------|
| `reuse_port` | `SO_REUSEPORT`. The primitive the engine scales on. |
| `reuse_addr` | `SO_REUSEADDR`. |
| `nonblocking` | `O_NONBLOCK`. A receive with nothing queued returns `would_block`. |
| `recv_buffer<Bytes>` | `SO_RCVBUF`. The kernel doubles the request and caps it at `net.core.rmem_max`. |
| `send_buffer<Bytes>` | `SO_SNDBUF`. Doubled and capped likewise. |
| `v6_only` | `IPV6_V6ONLY`. Worth setting explicitly when both families are bound separately, because the default is a sysctl. Fails with `invalid_argument` on a v4 socket. |
| `receive_metadata<Fs...>` | Ask the kernel to report each feature's control message. See [metadata](metadata.md). |

```cpp
auto sock{dgram::socket::open<dgram::reuse_port,
                              dgram::recv_buffer<1 << 20>>(dgram::family::inet4)};
```

Writing your own is one static function:

```cpp
struct my_option {
    static dgram::result<> apply(int fd, dgram::family fam) noexcept { /* setsockopt */ }
};
```
