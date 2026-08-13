#!/usr/bin/env bash
# Build clotho at two git refs as portable self-contained binaries, for the
# differential benchmark box (which has no LLVM / build toolchain).
#
# Uses a static config:
#   static LLVM install + -static-libstdc++ -static-libgcc  -> 4-lib binary,
#   glibc floor ~2.38 (runs on Ubuntu 24.04+). Only the clotho binary differs
#   between base and head; both link the same static LLVM, so the JIT-compile
#   metric is comparable side to side.
#
# Output: $OUT/clotho-base, $OUT/clotho-head  (ship these + tests/perf to the box).
#
#   cloud/build-refs.sh --base main --head my-feature
set -euo pipefail

REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
LLVM_STATIC=${LLVM_STATIC:?set LLVM_STATIC to a static LLVM install prefix}
DEPS=${DEPS:-$REPO/build-rel/_deps}         # reuse already-fetched slang/doctest/dense sources
OUT=${OUT:-$REPO/cloud/out}
WORK=${WORK:-$REPO/.bench-diff/build-refs}

base= head=
while [ $# -gt 0 ]; do
  case "$1" in
    --base) base=$2; shift 2;;
    --head) head=$2; shift 2;;
    *) echo "build-refs: unknown arg '$1'" >&2; exit 2;;
  esac
done
[ -n "$base" ] && [ -n "$head" ] || { echo "usage: build-refs.sh --base REF --head REF" >&2; exit 2; }

[ -f "$LLVM_STATIC/lib/cmake/llvm/LLVMConfig.cmake" ] || {
  echo "build-refs: static LLVM not found at $LLVM_STATIC (set LLVM_STATIC)" >&2; exit 2; }
[ -d "$DEPS/slang-src" ] || {
  echo "build-refs: fetched deps not found at $DEPS (build build-rel first, or set DEPS)" >&2; exit 2; }

mkdir -p "$OUT" "$WORK"

build_one() {
  local label=$1 ref=$2
  local sha; sha=$(git -C "$REPO" rev-parse --verify --quiet "${ref}^{commit}") || {
    echo "build-refs: unknown ref '$ref'" >&2; exit 2; }
  local wt="$WORK/wt-${sha:0:12}" bld
  bld="$wt/build-static"
  git -C "$REPO" worktree remove --force "$wt" 2>/dev/null || true
  echo "[build-refs] $label = $ref ($sha) -> worktree $wt"
  git -C "$REPO" worktree add --detach "$wt" "$sha"

  cmake -S "$wt" -B "$bld" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DLLVM_DIR="$LLVM_STATIC/lib/cmake/llvm" \
    -DSLANG_USE_MIMALLOC=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_tomlplusplus=ON \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    -DFETCHCONTENT_SOURCE_DIR_SLANG="$DEPS/slang-src" \
    -DFETCHCONTENT_SOURCE_DIR_DOCTEST="$DEPS/doctest-src" \
    -DFETCHCONTENT_SOURCE_DIR_UNORDERED_DENSE="$DEPS/unordered_dense-src" \
    >/dev/null
  ninja -C "$bld" clotho

  local bin="$OUT/clotho-$label"
  cp "$bld/clotho" "$bin"
  git -C "$REPO" worktree remove --force "$wt"

  # Portability gate: refuse a binary that pulled in more than the ubiquitous
  # four (libc, libm, libgcc_s, ld-linux). Anything else won't be on the box.
  local nlibs; nlibs=$(ldd "$bin" 2>/dev/null | grep -c '=>' || true)
  echo "[build-refs] $label -> $bin ($(du -h "$bin" | cut -f1), $nlibs dynamic deps)"
  ldd "$bin" 2>/dev/null | sed 's/^/    /' || true
  echo "$sha" > "$OUT/clotho-$label.sha"
}

build_one base "$base"
build_one head "$head"
echo "[build-refs] done. Ship $OUT/clotho-{base,head} + tests/perf/ to the box."
