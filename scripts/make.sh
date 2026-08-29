#!/usr/bin/env bash
# Configure, build and test datagram_engine. One build directory per
# configuration, so switching does not force a rebuild.
#
#   scripts/make.sh                     plain Debug build + tests
#   scripts/make.sh -s address          ASan + UBSan
#   scripts/make.sh -s thread           ThreadSanitizer
#   scripts/make.sh -H                  hardened (libstdc++ assertions, stack protection)
#   scripts/make.sh -f                  build and run the fuzzers
#   scripts/make.sh -a                  every defensive configuration in turn
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

sanitizer=none
build_type=Debug
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
  -n         configure and build only, do not run tests
  -a         run every defensive configuration in sequence
  -m <dir>   local libmem checkout (or set DGRAM_LIBMEM_DIR)
  -j <n>     parallel jobs
  -h         this help
USAGE
}

while getopts ":s:t:Hfnam:j:h" opt; do
    case "$opt" in
        s) sanitizer=$OPTARG ;;
        t) build_type=$OPTARG ;;
        H) hardened=ON ;;
        f) fuzzers=ON ;;
        n) run_tests=0 ;;
        a) all=1 ;;
        m) libmem_dir=$OPTARG ;;
        j) jobs=$OPTARG ;;
        h) usage; exit 0 ;;
        :) echo "option -$OPTARG needs an argument" >&2; exit 2 ;;
        *) echo "unknown option -$OPTARG" >&2; usage; exit 2 ;;
    esac
done

# One configuration: configure, build, test.
run_one() {
    local san=$1 hard=$2 fuzz=$3 type=$4
    local dir="build/${type,,}-${san//+/-}"
    [[ $hard == ON ]] && dir="${dir}-hardened"
    [[ $fuzz == ON ]] && dir="${dir}-fuzz"

    local args=(
        -S . -B "$dir" -G Ninja
        "-DCMAKE_BUILD_TYPE=$type"
        "-DDGRAM_SANITIZER=$san"
        "-DDGRAM_HARDENED=$hard"
        "-DDGRAM_BUILD_TESTS=ON"
        "-DDGRAM_BUILD_EXAMPLES=ON"
        "-DDGRAM_BUILD_FUZZERS=$fuzz"
    )
    [[ -n $libmem_dir ]] && args+=("-DFETCHCONTENT_SOURCE_DIR_LIBMEM=$libmem_dir")

    echo "==> ${dir}"
    cmake "${args[@]}" > /dev/null
    cmake --build "$dir" -j "$jobs"
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
