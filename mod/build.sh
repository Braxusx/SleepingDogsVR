#!/bin/sh
# SDVR build: llvm-mingw x86_64. Usage: ./build.sh <buildnum>
set -e
cd "$(dirname "$0")"
TC=${TC:-/home/claude/tc/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64/bin}   # llvm-mingw bin folder
N=${1:-001}
OUT=out; mkdir -p $OUT/obj
MH=third_party/minhook
NGX=${NGX:-/home/claude/ngx}   # NVIDIA DLSS SDK (headers + static lib), kept outside this tree
for f in $MH/src/buffer.c $MH/src/hook.c $MH/src/trampoline.c $MH/src/hde/hde64.c; do
  [ $OUT/obj/$(basename $f).o -nt $f ] || $TC/x86_64-w64-mingw32-clang -O2 -c $f -I$MH/include -o $OUT/obj/$(basename $f).o
done
CXXFLAGS="-O2 -std=c++17 -fno-threadsafe-statics -Wall -Wno-unused-function -Isrc -I$MH/include -Ithird_party/openxr -I$NGX/sdk/include -fms-extensions -DUNICODE -D_UNICODE -D__USE_MINGW_ANSI_STDIO=1"
for f in src/*.cpp; do
  $TC/x86_64-w64-mingw32-clang++ $CXXFLAGS -c $f -o $OUT/obj/$(basename $f).o
done
# NOTE: clang drops __try/__except around plain memory reads (SPatch's SafeRead); -Xclang -fasync-exceptions would keep them
# but crashes the compiler on files that also use C++ try. SdvrBridge.cpp installs a handler that covers those reads instead.
# SPatch (GPLv3, by xowny) - its own dll (sdvr_spatch.dll) with its own MinHook; only recompiled when a source is newer than its object
SPFLAGS="-O2 -std=c++20 -fms-extensions -w -Isrc/spatch/shim -Isrc/spatch -I$MH/include -D_WINDOWS -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DSPATCH_FINAL_RELEASE=1"
mkdir -p $OUT/obj/sp
for f in src/spatch/*.cpp; do
  o=$OUT/obj/sp/sp_$(basename $f).o
  [ $o -nt $f ] || $TC/x86_64-w64-mingw32-clang++ $SPFLAGS -c $f -o $o
done
$TC/x86_64-w64-mingw32-clang++ -shared -s -o $OUT/dinput8_$N.dll $OUT/obj/*.cpp.o $OUT/obj/*.c.o src/dinput8.def \
  $NGX/sdk/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib -L$NGX/dummy -static -ladvapi32 -lcrypt32 -lbcrypt -lpsapi -lshlwapi -luser32 -ld3d11 -ldxgi -luuid -Wl,--kill-at
$TC/x86_64-w64-mingw32-clang++ -shared -s -o $OUT/sdvr_spatch_$N.dll $OUT/obj/sp/*.o $OUT/obj/*.c.o \
  -static -ladvapi32 -lcrypt32 -lbcrypt -lpsapi -lshlwapi -luser32 -ld3d11 -ldxgi -luuid -Wl,--kill-at
md5sum $OUT/dinput8_$N.dll $OUT/sdvr_spatch_$N.dll
