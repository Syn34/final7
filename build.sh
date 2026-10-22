#!/bin/sh
# Cross-compile final7.exe on Linux/macOS with MinGW-w64
#   (Debian/Ubuntu: sudo apt install gcc-mingw-w64-x86-64)
set -e
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
mkdir -p build
$WINDRES src/app.rc -O coff -o build/app.res
$CC -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -s -static -mwindows \
    src/ui.c src/game.c src/emu.c src/demo.c src/art.c src/stars.c src/cards_data.c build/app.res \
    -o final7.exe -lcomctl32 -luxtheme -lgdiplus -lshlwapi -lole32 -lcomdlg32 -lgdi32 -luser32 -lm
echo "Built final7.exe"
