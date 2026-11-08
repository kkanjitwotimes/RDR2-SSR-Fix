#!/usr/bin/env bash
# Cross-compiles RDR2OverscanVK.asi (Windows x64) with the llvm-mingw toolchain. No network needed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
TC="${TC:-/home/claude/deps/toolchain}"
VKH="${VKH:-/home/claude/deps/vulkan-headers/include}"
DET="${DET:-/home/claude/deps/detours/src}"
OUT="$ROOT/build"; mkdir -p "$OUT"
export PATH="$TC/bin:$PATH"
CXX=x86_64-w64-mingw32-clang++
FLAGS=(-std=c++17 -O2 -DVK_NO_PROTOTYPES -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DDETOURS_X64 -D_WIN32_WINNT=0x0A00
       -I"$VKH" -I"$DET" -I"$ROOT/src" -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type-mismatch -Wno-cast-function-type)
DETFLAGS=(-std=c++17 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DDETOURS_X64 -I"$DET" -w)
for f in detours disasm modules image creatwth; do
  [ "$OUT/$f.o" -nt "$DET/$f.cpp" ] || $CXX "${DETFLAGS[@]}" -c "$DET/$f.cpp" -o "$OUT/$f.o"
done
$CXX "${FLAGS[@]}" -shared -static "$ROOT/src/rdr2_overscan.cpp" "$OUT"/{detours,disasm,modules,image,creatwth}.o \
     -o "$OUT/RDR2OverscanVK.asi" -luser32 -ladvapi32 -Wl,--dynamicbase -Wl,--nxcompat
ls -la "$OUT/RDR2OverscanVK.asi"; file "$OUT/RDR2OverscanVK.asi"
