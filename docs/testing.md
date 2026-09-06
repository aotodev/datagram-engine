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
| `doc_examples.cpp` | Every snippet printed in `README.md` and `docs/`, compiled and run. A snippet in a document is untested code that looks authoritative; this is what stops one rotting. |
| `compile_checks.cpp` | Pure `static_assert`. Feature concepts, control-buffer sizing, batch geometry, which resources may be carved from, which metadata is readable. Compiling is the test; it is never run. |
| `loopback_tests.cpp` | Batch reuse, truncation reporting, nonblocking receive, staging limits, a failed flush leaving the batch staged, the error combinators. |
| `metadata_tests.cpp` | Destination address and ECN over loopback, both families, including a feature that was never enabled and an undersized control buffer. |
| `malformed_control_tests.cpp` | Control buffers the kernel would never write. Every case here was found by the fuzzer first. |
| `offload_tests.cpp` | GRO and GSO over loopback, and the segmentation walk on its own: short tail, exact multiple, stride of zero, stride past the end, empty payload. |
| `demux_tests.cpp` | Key equality and hash agreement (including the flow-label and padding cases), the flow table under churn and at capacity, projections, routing a GRO slot whose datagrams belong to different flows, an `unmatched` callback claiming or declining a datagram, arguments forwarded to a sink that takes them and ignored by one that does not, and the per-slot count. |
| `timer_tests.cpp` | The timing wheel: every delay across every level boundary, stepped one tick at a time and jumped, from several epochs, plus a differential run against a naive reference. `next_deadline` gets its own differential run under churn, the structural case of a timer wrapped onto the cursor's slot, and the property a waiting loop needs: waking at the deadline always finds work. |
| `pacing_tests.cpp` | The rate arithmetic exhaustively (monotonicity, drift, extreme rates, constexpr), plus what the kernel side allows unprivileged. Real pacing needs `fq` and `CAP_NET_ADMIN`, so no test here asserts a datagram was actually delayed. |
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

Two harnesses, both over the paths where kernel-supplied values drive pointer
arithmetic.

`fuzz_cmsg_parse` drives `parse_control` over an arbitrary control buffer: the
`CMSG_*` macros walk it using lengths taken from the buffer itself.

`fuzz_segments` drives the GRO segmentation walk, whose stride comes from a
control message. It asserts the properties a wrong stride would break: every
segment lies inside the buffer, segments are contiguous and ordered, and together
they cover the payload exactly once.

`fuzz_timer` drives the timing wheel differentially against a naive reference,
across random epochs, delays and step sizes. The wheel is not attacker-facing,
but its cascade only runs on wrap ticks, which is the shape that is right for
most delays and wrong for a few.

`fuzz_demux` drives the payload key projection, which reads bytes at an offset
out of an attacker-supplied datagram, and drives the flow table differentially
against a reference map. The table is not attacker-facing, but its probe chains
and backward-shift deletion are index arithmetic that can be wrong in one corner
and right everywhere else.

```sh
scripts/make.sh -f                                   # standalone driver, runs under ctest
DGRAM_FUZZ_ITERATIONS=1000000 ./build/debug-address-undefined-hardened-fuzz/fuzz/fuzz_cmsg_parse
```

## Why AFL++, and why built from source

libFuzzer is not available here. It is a Clang feature, GCC has no equivalent,
and Clang cannot compile this library at all: it supports neither expansion
statements nor contracts. The coverage-guided path is therefore AFL++, whose
compilers are GCC plugins.

**Do not use a packaged AFL++.** Its GCC plugin is ABI-tied to the exact compiler
it was built against, so a distro package and a rolling GCC drift apart within
weeks and every build then dies at:

```
PROGRAM ABORT : GCC and plugin have incompatible versions, expected GCC 16.1.1, is 16.2.1
```

Building AFL++ from source against the compiler that is actually present makes
that skew impossible rather than merely unlikely. Locally:

```sh
scripts/get-afl.sh          # clones a pinned tag, builds into .deps/afl
scripts/make.sh -A -s address+undefined -H
```

`get-afl.sh` is idempotent and re-checks the existing build against the current
GCC, so re-running it after a compiler upgrade does the right thing. Both it and
`make.sh -A` verify the compiler accepts a real translation unit before
committing to a build.

In CI the same reasoning puts AFL++ in the toolchain image, built from source in
the same layer as GCC (`ci/Containerfile`), so the two cannot disagree. The image
build fails immediately if the plugin does not load, rather than a fuzz job
failing three weeks later.

Driving a harness:

```sh
AFL_PATH=$PWD/.deps/afl/lib/afl .deps/afl/bin/afl-fuzz \
    -i fuzz/corpus -o out -V 300 -- build/<config>-afl/fuzz/fuzz_cmsg_parse
```

Without AFL++ the same harness links `fuzz_driver.cpp`'s standalone path, which
replays the corpus and then mutates it under a fixed seed. **That is not
coverage-guided** and finds less, but it runs everywhere the library builds and
is what `ctest` executes.

Two properties of the harnesses carry most of their value, and both were learned
the hard way:

- The buffer is allocated to **exactly** the input length. Reusing an oversized
  static buffer hides a short overread inside the allocation, where no sanitizer
  sees it.
- The corpus is **seeded with well-formed control messages** at the widths the
  kernel really uses. Random bytes essentially never produce a `cmsg_len` above
  `sizeof(cmsghdr)` paired with a level and type a feature answers to, so an
  unseeded corpus never reaches a parser at all.

Findings are pinned as unit tests in `malformed_control_tests.cpp` rather than
left to a fuzz run.

## Editor support

clangd works, partially, and needs one flag that cannot be set in `.clangd`:

```
clangd --experimental-modules-support
```

Without it every `import` is an unresolved-module error. GCC's `.gcm` files are
not readable by clang, so clangd has to build its own BMIs from source, and it
only does that when asked.

`.clangd` handles the rest: it strips the GCC driver flags clang rejects, which
matters more than it looks. An unknown argument such as `-fcontracts` aborts
clangd's module dependency scan *before* it parses anything, so one stray flag
turns every file red for a reason that has nothing to do with the file.

### What clangd cannot do

Two constructs clang 22 does not implement, in two files:

| File | Construct |
|------|-----------|
| `src/dgram/cmsg.cppm` | one `pre(...)` contract |
| `src/dgram/feature.cppm` | two `template for` expansion statements |

Everything importing them inherits the failure, which in practice is the whole
library. Files that avoid both (`address`, `error`, `socket`) check clean.

This is a gap in clang, not a defect here, and it closes when clang implements
the two papers. Building the code is the diagnostic that always works, and a
full build is a few seconds.
