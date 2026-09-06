# Using dgram from another project

Two ways in. Fetch it as a subproject:

```cmake
include(FetchContent)
FetchContent_Declare(datagram-engine
    GIT_REPOSITORY https://github.com/aotodev/datagram-engine.git
    GIT_TAG master
    SYSTEM
)
FetchContent_MakeAvailable(datagram-engine)

target_link_libraries(my_target PRIVATE dgram::dgram)
```

Or install it once and find it:

```cmake
list(APPEND CMAKE_MODULE_PATH "${CMAKE_INSTALL_PREFIX}/lib/cmake/dgram")
include(enable_standard_modules)   # before project(): it sets the import std UUID
enable_experimental_std()

project(my_project LANGUAGES CXX)
set(CMAKE_CXX_MODULE_STD ON)

find_package(dgram 0.9.0 REQUIRED)
target_link_libraries(my_target PRIVATE dgram::dgram)
```

`dgram::dgram` names the target either way; the bare `dgram` also works from a
subproject build, where it is a real target rather than an alias.

Then `import dgram;`. The module carries `libmem` transitively, so
`import libmem;` is available too and is needed for the arena.

## What an install puts down

`cmake --install` writes both packages, because a consumer builds its own BMIs
and dgram's interfaces `import libmem`:

| Path | What |
|------|------|
| `lib/libdgram.a`, `lib/liblibmem.a` | the archives |
| `share/dgram/modules/`, `share/libmem/modules/` | module interfaces, as **sources** |
| `lib/cmake/dgram/`, `lib/cmake/libmem/` | the package configs and export sets |
| `lib/cmake/dgram/enable_standard_modules.cmake` | needed before your `project()`, so it ships loose |

**No BMI is installed.** A BMI is only valid for the exact flag set that built
it, so shipping one would be shipping a landmine; the consumer compiles the
installed interfaces under its own flags. That is also why the codegen rule
below applies to a `find_package` build exactly as it does to a fetched one.

`find_package(dgram)` pulls in `libmem` through `find_dependency`, so a
consumer names one package rather than two. Version compatibility is
`SameMinorVersion`: before 1.0 a minor bump is a break.

## Static or shared

`BUILD_SHARED_LIBS` picks the type, as it does for any CMake library. Shared
gets a soname (`libdgram.so.0.9`), and `libmem` is forced position independent so
a static one can still land inside it.

**Upgrade the library and the consumer together.** A consumer compiles the
installed module interfaces itself, so templates and inline entities are baked
into its own objects while the rest come from the library. Dropping in a
different `libdgram.so` against objects built from the old interfaces is an ODR
mismatch that nothing diagnoses. The soname is not an ABI promise: there is no
version script and no ABI discipline for module-attached entities yet.

Symbol visibility is left at the default rather than hidden. Hiding it would mean
annotating exported entities across twelve module interfaces, and it would buy a
smaller symbol table on a library whose consumers compile most of it anyway.

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
| `DGRAM_INSTALL` | top-level only | Install rules and the `find_package` config |
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
