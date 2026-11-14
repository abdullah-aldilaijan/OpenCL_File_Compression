#!/bin/sh
# Builds with llvm-mingw clang (used when MSVC is not installed).
# Usage: sh build-clang.sh [output-suffix]   e.g. "_test" -> build/oclzip_test.exe
set -e
cd "$(dirname "$0")"
CXX="${CXX:-$(ls .tools/*/bin/clang++.exe | head -1)}"
SUFFIX="${1:-}"
mkdir -p build
CORE="src/common.cpp src/ocl.cpp src/hardware.cpp src/zip_create.cpp src/zip_restore.cpp src/legacy.cpp src/pack.cpp"
FLAGS="-std=c++20 -O2 -march=x86-64-v2 -Wall -Wextra -Wno-missing-field-initializers -static -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00"
"$CXX" $FLAGS src/cli.cpp $CORE -o "build/oclzip$SUFFIX.exe" -lbcrypt -ladvapi32 -luser32 -lsetupapi -lcfgmgr32 -lcabinet
"$CXX" $FLAGS -mwindows src/gui.cpp $CORE -o "build/oclzip-gui$SUFFIX.exe" -lbcrypt -ladvapi32 -luser32 -lsetupapi -lcfgmgr32 -lcabinet -lgdi32 -lcomctl32 -lole32 -lshell32 -luuid
echo "Built build/oclzip$SUFFIX.exe and build/oclzip-gui$SUFFIX.exe"
