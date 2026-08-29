#!/usr/bin/env bash
# Build AFL++ from source against the GCC currently on PATH, into a
# project-local, gitignored prefix.
#
# The distro package will not do. AFL++'s GCC plugin is ABI-tied to the exact
# compiler it was built against, so a packaged AFL++ and a rolling GCC drift
# apart and afl-g++-fast then aborts with:
#
#   PROGRAM ABORT : GCC and plugin have incompatible versions
#
# Building it here, now, against the compiler that is actually installed makes
# that impossible. Re-run it after a GCC upgrade.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

readonly version=${AFL_VERSION:-v5.02c}
readonly prefix=${AFL_PREFIX:-$PWD/.deps/afl}
readonly src=$PWD/.deps/AFLplusplus

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

if [[ -x $prefix/bin/afl-g++-fast ]] && [[ ${1:-} != --force ]]; then
    if afl_matches "$prefix/bin/afl-g++-fast"; then
        echo "AFL++ already built and matching this GCC: $prefix/bin"
        exit 0
    fi
    echo "existing AFL++ does not match this GCC, rebuilding"
fi

mkdir -p "$(dirname "$src")"
if [[ -d $src/.git ]]; then
    git -C "$src" fetch --depth 1 origin "refs/tags/$version:refs/tags/$version"
    git -C "$src" checkout -q "$version"
else
    git clone --depth 1 --branch "$version" https://github.com/AFLplusplus/AFLplusplus.git "$src"
fi

# The gcc_plugin target needs the core binaries (afl-showmap) to exist first.
make -C "$src" all -j"$(nproc)"
make -C "$src" gcc_plugin -j"$(nproc)"
make -C "$src" install PREFIX="$prefix"

afl_matches "$prefix/bin/afl-g++-fast" || {
    echo "built AFL++ still does not accept this GCC" >&2
    exit 1
}

cat <<EOF

AFL++ $version built against $(gcc --version | head -1)
  prefix: $prefix

Use it with:
  scripts/make.sh -A
EOF
