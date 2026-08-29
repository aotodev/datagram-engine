# Errors

## `result` and `errc`

```cpp
enum class errc : int {};                    // a POSIX errno, never zero
template <typename T = void>
using result = std::expected<T, errc>;
```

Everything that can fail at the syscall boundary returns a `result`. Nothing
throws. A `result` holding an error always holds a real failure, so
`has_value()` is the only success test needed.

`std::string_view describe(errc)` gives human-readable text.

## Named constants

`errno` values are macros, and macros do not cross a module boundary, so
`import dgram;` alone would not let you name them. The ones a caller routinely
branches on are exported:

`interrupted`, `would_block`, `out_of_memory`, `invalid_argument`,
`message_too_long`, `connection_refused`, `address_in_use`.

```cpp
if (const auto got{rx.receive(sock)}; !got) {
    if (got.error() == dgram::would_block) { return; }
    std::println(stderr, "receive: {}", dgram::describe(got.error()));
}
```

For anything else, construct it: `dgram::errc{ENOTCONN}` with `<cerrno>`
included.

## Combinators

`std::expected` carries `and_then`, `transform` and `or_else` as members, which
compose right to left. These wrap them so a chain reads in the order it runs.

| Combinator | Wraps | Use |
|------------|-------|-----|
| `then(f)` | `and_then` | `f` returns a `result`. |
| `map(f)` | `transform` | `f` returns a plain value. |
| `recover(f)` | `or_else` | `f` takes an `errc` and returns a `result`. |
| `tap(f)` | `transform` | Run `f` for its effect, pass the value through. |

```cpp
const auto port = dgram::socket::open<dgram::reuse_port>(dgram::family::inet4)
                | dgram::then([](auto&& s) { return s.bind(local).transform([&] { return std::move(s); }); })
                | dgram::then([](auto&& s) { return s.local_address(); })
                | dgram::map(&dgram::endpoint::port);
```

`tap` yields by value, because `expected` cannot hold a reference. It does not
run on the error path.

## `invoke_syscall`

```cpp
template <typename F, typename... Args>
auto invoke_syscall(F&& f, Args&&... args) -> result<std::invoke_result_t<F, Args...>>;
```

Wraps a syscall that reports failure as a negative return, converting it to
`errc{errno}`. `EINTR` is not retried: a caller that parks on readiness wants to
see it, and one that does not can retry at its own layer.
