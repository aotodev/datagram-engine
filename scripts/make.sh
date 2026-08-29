#!/usr/bin/env bash
# Configure, build and test datagram_engine. One build directory per
# configuration, so switching does not force a rebuild.
#
#   scripts/make.sh                     plain Debug build + tests
#   scripts/make.sh -s address          ASan + UBSan
#   scripts/make.sh -s thread           ThreadSanitizer
#   scripts/make.sh -H                  hardened (libstdc++ assertions, stack protection)
#   scripts/make.sh -f                  build and run the fuzzers
#   scripts/make.sh -A                  coverage-guided fuzzing with AFL++
#   scripts/make.sh -a                  every defensive configuration in turn
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

sanitizer=none
build_type=Debug
use_afl=0
afl_prefix=${AFL_PREFIX:-$PWD/.deps/afl}
hardened=OFF
fuzzers=OFF
run_tests=1
all=0
libmem_dir=${DGRAM_LIBMEM_DIR:-}
jobs=$(nproc)

usage() {
    sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    cat <<'USAGE'

Options:
  -s <san>   none | address | undefined | address+undefined | thread
  -t <type>  CMAKE_BUILD_TYPE (default Debug)
  -H         hardened build
  -f         build and run fuzzers
  -A         build the fuzzers with AFL++ (implies -f); run scripts/get-afl.sh first
  -n         configure and build only, do not run tests
  -a         run every defensive configuration in sequence
  -m <dir>   local libmem checkout (or set DGRAM_LIBMEM_DIR)
  -j <n>     parallel jobs
  -h         this help
USAGE
}

while getopts ":s:t:HfAnam:j:h" opt; do
    case "$opt" in
        s) sanitizer=$OPTARG ;;
        t) build_type=$OPTARG ;;
        H) hardened=ON ;;
        f) fuzzers=ON ;;
        A) fuzzers=ON; use_afl=1 ;;
        n) run_tests=0 ;;
        a) all=1 ;;
        m) libmem_dir=$OPTARG ;;
        j) jobs=$OPTARG ;;
        h) usage; exit 0 ;;
        :) echo "option -$OPTARG needs an argument" >&2; exit 2 ;;
        *) echo "unknown option -$OPTARG" >&2; usage; exit 2 ;;
    esac
done

# Compile a real translation unit: the plugin's version check runs when GCC
# loads it, and an empty file would fail at link for want of a main instead.
afl_matches() {
    local cxx=$1 tmp
    tmp=$(mktemp -d)
    echo 'int main(){return 0;}' > "$tmp/probe.cpp"
    local ok=1
    AFL_QUIET=1 "$cxx" -c "$tmp/probe.cpp" -o "$tmp/probe.o" >/dev/null 2>&1 || ok=0
    rm -rf "$tmp"
    return $(( ! ok ))
}

# One configuration: configure, build, test.
run_one() {
    local san=$1 hard=$2 fuzz=$3 type=$4
    local dir="build/${type,,}-${san//+/-}"
    [[ $hard == ON ]] && dir="${dir}-hardened"
    [[ $fuzz == ON ]] && dir="${dir}-fuzz"

    if (( use_afl )); then
        dir="${dir}-afl"
    fi

    local args=(
        -S . -B "$dir" -G Ninja
        "-DCMAKE_BUILD_TYPE=$type"
        "-DDGRAM_SANITIZER=$san"
        "-DDGRAM_HARDENED=$hard"
        "-DDGRAM_BUILD_TESTS=$([[ $use_afl == 1 ]] && echo OFF || echo ON)"
        "-DDGRAM_BUILD_EXAMPLES=$([[ $use_afl == 1 ]] && echo OFF || echo ON)"
        "-DDGRAM_BUILD_FUZZERS=$fuzz"
    )
    [[ -n $libmem_dir ]] && args+=("-DFETCHCONTENT_SOURCE_DIR_LIBMEM=$libmem_dir")

    if (( use_afl )); then
        local afl_cxx=$afl_prefix/bin/afl-g++-fast
        [[ -x $afl_cxx ]] || afl_cxx=$(command -v afl-g++-fast || true)
        if [[ -z $afl_cxx ]]; then
            echo "afl-g++-fast not found; run scripts/get-afl.sh" >&2
            exit 1
        fi
        # A packaged AFL++ built against a different GCC aborts at compile time,
        # so check now rather than a hundred targets in.
        if ! afl_matches "$afl_cxx"; then
            echo "$afl_cxx does not match this GCC; run scripts/get-afl.sh" >&2
            exit 1
        fi
        args+=("-DCMAKE_CXX_COMPILER=$afl_cxx")
    fi

    echo "==> ${dir}"
    cmake "${args[@]}" > /dev/null
    cmake --build "$dir" -j "$jobs"
    if (( use_afl )); then
        echo "AFL++ build ready. Drive a harness with:"
        echo "  afl-fuzz -i fuzz/corpus -o out -V 300 -- $dir/fuzz/fuzz_cmsg_parse"
        return
    fi
    if (( run_tests )); then
        ctest --test-dir "$dir" --output-on-failure -j "$jobs"
    fi
}

if (( all )); then
    # TSan is separate from ASan: the two cannot share a process. Fuzzing runs
    # under ASan+UBSan, where an overread actually lands in a redzone.
    run_one none              OFF OFF Release
    run_one address+undefined ON  OFF Debug
    run_one thread            OFF OFF Debug
    run_one address+undefined ON  ON  Debug
    echo "all defensive configurations passed"
else
    run_one "$sanitizer" "$hardened" "$fuzzers" "$build_type"
fi
