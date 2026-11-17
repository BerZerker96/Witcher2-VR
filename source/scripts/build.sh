#!/bin/bash
# Builds W2VR (32-bit xinput1_3.dll for witcher2.exe) with MinGW-w64 (Debian/Ubuntu: apt install mingw-w64).
#   scripts/build.sh   -> build/xinput1_3.dll
set -e
cd "$(dirname "$0")/.."
CXX=${CXX:-i686-w64-mingw32-g++-posix}; CC=${CC:-i686-w64-mingw32-gcc-posix}
OUT=build
mkdir -p $OUT/obj
V=third_party/vrkit; I=$V/third_party/imgui; MH=third_party/minhook/src
INC="-Isrc -Ithird_party/minhook/include -Ithird_party/xrbridge/include -I$V -I$I"
# -mincoming-stack-boundary=2: the mod's hooks are called from game code that only keeps 4-byte stack alignment
CF="-O2 -g -msse2 -mfpmath=sse -mincoming-stack-boundary=2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-cast-function-type"
$CC $CF $INC -c src/vtcheck.c -o $OUT/obj/vtcheck.o    # static asserts only
OBJS=""
for f in src/*.cpp third_party/xrbridge/src/client/client.cpp $V/vrkit_input.cpp $V/vrkit_menu_core.cpp $V/vrkit_options.cpp \
         $V/vrkit_menu_d3d9.cpp $I/imgui.cpp $I/imgui_draw.cpp $I/imgui_tables.cpp $I/imgui_widgets.cpp $I/imgui_impl_dx9.cpp; do
  o=$OUT/obj/$(basename ${f%.*}).o; $CXX -std=c++17 $CF $INC -include src/realtime.h -c $f -o $o; OBJS="$OBJS $o"
done
for f in $MH/buffer.c $MH/hook.c $MH/trampoline.c $MH/hde/hde32.c; do
  o=$OUT/obj/mh_$(basename ${f%.*}).o; $CC $CF $INC -c $f -o $o; OBJS="$OBJS $o"
done
$CXX -shared -o $OUT/xinput1_3.dll $OBJS src/xinput1_3.def -static -static-libgcc -static-libstdc++ \
     -Wl,--enable-stdcall-fixup -Wl,--kill-at -Wl,--image-base,0x6A800000 -ld3d9 -lwinmm -luser32 -lshell32 -ladvapi32 -lole32
i686-w64-mingw32-strip -o $OUT/xinput1_3_stripped.dll $OUT/xinput1_3.dll
ls -la $OUT/*.dll
