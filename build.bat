@echo off
rem Builds final7.exe with MinGW-w64 (e.g. from https://winlibs.com) on Windows.
cd /d "%~dp0"
if not exist build mkdir build
windres src\app.rc -O coff -o build\app.res || goto :err
gcc -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -s -static -mwindows src\ui.c src\game.c src\emu.c src\demo.c src\art.c src\stars.c src\cards_data.c build\app.res -o final7.exe -lcomctl32 -luxtheme -lgdiplus -lshlwapi -lole32 -lcomdlg32 -lgdi32 -luser32 -lm || goto :err
echo Built final7.exe
exit /b 0
:err
echo Build failed.
exit /b 1
