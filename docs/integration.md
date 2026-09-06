# Using dgram from another project

```cmake
include(FetchContent)
FetchContent_Declare(datagram-engine
    GIT_REPOSITORY https://github.com/aotodev/datagram-engine.git
    GIT_TAG master
    SYSTEM
)
FetchContent_MakeAvailable(datagram-engine)

target_link_libraries(my_target PRIVATE dgram)
```

Then `import dgram;`. The module carries `libmem` transitively, so
`import libmem;` is available too and is needed for the arena.

## Requirements

GCC >= 16, CMake >= 3.30, Ninja, Linux. The engine is built on `recvmmsg`,
`cmsg`, GRO/GSO and `SO_TXTIME`, so there is no portable subset to fall back on.

## Codegen flags are the parent's job

`import std;` builds the standard library BMI from the flags of whatever target
first needs it. Set sanitizers, LTO and `-fno-rtti` at directory scope, before
any dependency is configured, so the whole build agrees. A per-target
`-fsanitize` forks the std BMI and every importer then fails with
`import 'std' has CRC mismatch`.

dgram applies its own instrumentation at directory scope and forces libmem's
`USE_SANITIZERS` off, so there is exactly one source of sanitizer flags. libmem
applies its own per-config through an interface target, which reaches every real
target but not CMake's synthesised `import std` one; in a Debug build that
asymmetry alone is a CRC mismatch.

## Options

| Option | Default | Effect |
|--------|---------|--------|
| `DGRAM_BUILD_TESTS` | `OFF` | Build the GoogleTest suite and the compile checks |
| `DGRAM_BUILD_EXAMPLES` | `OFF` | Build the examples |
| `USE_SANITIZERS` | `OFF` | ASan + UBSan, directory-wide |

`libmem` is fetched automatically. Point at a local checkout with
`-DFETCHCONTENT_SOURCE_DIR_LIBMEM=/path/to/libmem`.

The engine pins `libmem` to a release tag, so that half is reproducible. **The
snippet above is not:** `GIT_TAG master` means a build is only as reproducible as
the engine's master was that day, so pin a tag or a commit if two machines have
to build the same thing.

A local checkout is the hazard from the other side. libmem's compile options
reach the `import std` BMI, so a stale one forks it, and CMake then rebuilds
libmem's module interfaces under a second flag set. That surfaces as
`'libmem::aligned_monotonic_resource' has not been declared`, or as `-Werror`
inside libstdc++ headers, neither of which points anywhere near the cause. Pull
it, or drop the override and take the pinned tag.

## Sizing

Every batch reports a `constexpr footprint()`. Sum them, make one arena that
size, and carve before the loop starts:

```cpp
using rx = dgram::receive_batch<64, 2048>;
using tx = dgram::transmit_batch<64>;

libmem::arena arena{rx::footprint() + tx::footprint()};
```

One socket, one arena and one batch pair per thread behind `SO_REUSEPORT`.
Nothing in the engine is thread-safe and nothing needs to be, because a batch
never crosses a thread boundary.
