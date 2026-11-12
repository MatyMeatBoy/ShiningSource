#!/bin/sh
# Build and run the save-protection test with the host compiler (SHININGSOURCE_LLVM_MINGW or clang in PATH).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${SHININGSOURCE_LLVM_MINGW:-D:/01Claude/toolchain/llvm-mingw-20261006-ucrt-x86_64}/bin/clang
[ -x "$CC" ] || [ -x "$CC.exe" ] || CC=clang
mkdir -p "$ROOT/build/tests" && cd "$ROOT/build/tests" || exit 1
"$CC" -O1 -w -I "$ROOT/runtime/include" "$ROOT/tools/tests/save_test.c" -o save_test.exe && ./save_test.exe
