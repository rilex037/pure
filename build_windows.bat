@echo off
rem PurePlayer - build script (Windows, MinGW-w64 + FFmpeg shared dev + SDL3 static)
rem Needs: MinGW-w64 gcc in PATH,
rem        an FFmpeg *shared* dev build unpacked into .\ffmpeg  (include, lib, bin)
rem        SDL3 for MinGW unpacked into .\sdl3                  (include, lib\libSDL3.a)
rem After building, copy .\ffmpeg\bin\{avcodec,avformat,avutil,swscale,swresample}-*.dll
rem next to PurePlayer.exe. SDL3 is linked statically: no SDL3.dll needed.

setlocal
where gcc >nul 2>nul
if errorlevel 1 (
    echo ERROR: gcc not found in PATH.
    exit /b 1
)
if not exist ffmpeg\include\libavformat\avformat.h (
    echo ERROR: FFmpeg dev files not found in .\ffmpeg
    exit /b 1
)
if not exist sdl3\include\SDL3\SDL.h (
    echo ERROR: SDL3 dev files not found in .\sdl3
    exit /b 1
)
if not exist sdl3\lib\libSDL3.a (
    echo ERROR: .\sdl3\lib\libSDL3.a missing ^(static SDL3 build, see Makefile^)
    exit /b 1
)

windres assets\app.rc -O coff -o app_res.o
if errorlevel 1 (
    echo RESOURCE BUILD FAILED
    exit /b 1
)

gcc -O2 -mwindows -static-libgcc -Wall -ffunction-sections -fdata-sections ^
    -I src -I ffmpeg\include -I sdl3\include ^
    src\ff.c src\ui_core.c src\ui_draw.c src\plat.c app_res.o ^
    -o PurePlayer.exe -Wl,--gc-sections -s ^
    ffmpeg\lib\avformat.lib ffmpeg\lib\avcodec.lib ^
    ffmpeg\lib\swscale.lib ffmpeg\lib\swresample.lib ffmpeg\lib\avutil.lib ^
    sdl3\lib\libSDL3.a ^
    -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion ^
    -luuid -ladvapi32 -lsetupapi -lshell32 -lcfgmgr32 -lcomdlg32

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo Built PurePlayer.exe
endlocal
