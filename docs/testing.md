# Testing

```sh
scripts/make.sh              # Debug, no sanitizer, build + ctest
scripts/make.sh -s address+undefined -H
scripts/make.sh -s thread
scripts/make.sh -f           # build and run the fuzzers
scripts/make.sh -a           # every defensive configuration in sequence
```

Each configuration gets its own build directory, so switching between them does
not force a rebuild. `-m <dir>` (or `DGRAM_LIBMEM_DIR`) points at a local libmem
checkout.

## What the suites cover

| Suite | Covers |
|-------|--------|
| `compile_checks.cpp` | Pure `static_assert`. Feature concepts, control-buffer sizing, batch geometry, which resources may be carved from, which metadata is readable. Compiling is the test; it is never run. |
| `loopback_tests.cpp` | Batch reuse, truncation reporting, nonblocking receive, staging limits, the error combinators. |
| `metadata_tests.cpp` | Destination address and ECN over loopback, both families, including a feature that was never enabled and an undersized control buffer. |
| `malformed_control_tests.cpp` | Control buffers the kernel would never write. Every case here was found by the fuzzer first. |
| `concurrency_tests.cpp` | Several workers, each with its own socket, arena and batches, running concurrently. Meaningful only under `-s thread`. |

## Configurations

**ASan + UBSan** is the default defensive build. It has already caught a
use-after-scope in a test and confirmed a heap overread in the parser.

**ThreadSanitizer** exists for one claim: the engine is shared-nothing, one
socket and one arena per thread behind `SO_REUSEPORT`, and nothing in it is
synchronised. That is only worth what a race detector says about it, which is
what `concurrency_tests.cpp` asks. TSan cannot share a process with ASan, so it
is a separate configuration.

**Hardened** (`-H`) turns on `_GLIBCXX_ASSERTIONS`, stack protection, stack-clash
protection and pattern-initialised locals, plus `_FORTIFY_SOURCE=3` where
optimisation makes it meaningful. `_GLIBCXX_ASSERTIONS` is the valuable one: it
makes a bad `span` index or `subspan` a trap rather than a silent read, and this
engine is built out of span arithmetic.

These are compile flags, so they must be identical across the `import std` BMI,
libmem and dgram. They are applied at directory scope before any dependency is
configured, for exactly that reason.

## Fuzzing

`fuzz/fuzz_cmsg_parse.cpp` drives `parse_control` over an arbitrary control
buffer. That parser is the only place in the engine where attacker-controlled
bytes meet pointer arithmetic: the `CMSG_*` macros walk the buffer using lengths
taken from the buffer itself.

```sh
scripts/make.sh -f
DGRAM_FUZZ_ITERATIONS=1000000 ./build/debug-address-undefined-hardened-fuzz/fuzz/fuzz_cmsg_parse
```

The harness is libFuzzer-shaped, but **libFuzzer is not available here**: it is a
Clang feature, GCC has no equivalent, and Clang cannot compile this library at
all (it supports neither expansion statements nor contracts). The coverage-guided
path is therefore AFL++, whose compilers are GCC plugins; the build detects
`afl-g++-fast` and uses it when present.

Without AFL++ the same harness links `fuzz_driver.cpp`, which replays a corpus
and then mutates it under a fixed seed. **That is not coverage-guided** and finds
less. It is still worth running, because it runs everywhere the library builds
and under whichever sanitizer the build selected.

Two properties of the harness carry most of its value, and both were learned the
hard way:

- The control buffer is allocated to **exactly** the input length. Reusing an
  oversized static buffer hides a short overread inside the allocation, where no
  sanitizer sees it.
- The corpus is **seeded with well-formed control messages** at the widths the
  kernel really uses. Random bytes essentially never produce a `cmsg_len` above
  `sizeof(cmsghdr)` paired with a level and type a feature answers to, so an
  unseeded corpus never reaches a parser at all.

Findings are pinned as unit tests in `malformed_control_tests.cpp` rather than
left to a fuzz run.
