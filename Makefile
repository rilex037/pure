# PurePlayer - Makefile (Windows + Linux, FFmpeg + SDL3)
#
# Linux (Mint/Ubuntu):   make                (needs pkg-config files for FFmpeg + sdl3)
# Windows (MinGW-w64):   make                (FFmpeg dev files in ./ffmpeg, SDL3 mingw dev files in ./sdl3)
# Cross build on Linux:  make CROSS=x86_64-w64-mingw32-
#
# Static SDL3 in a private prefix:  make PKG_CONFIG_PATH=$HOME/sdl3/lib/pkgconfig
CROSS   ?=
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres
FFMPEG  := ffmpeg
SDL     := sdl3
SRC     := src/ff.c src/ui_core.c src/ui_draw.c src/plat.c
HDR     := src/player.h src/stb_truetype.h
CFLAGS  := -O2 -Wall -Isrc -I/home/rilex/Downloads/PurePlayer-1.0/sdl3-prefix/include -ffunction-sections -fdata-sections
LDFLAGS := -Wl,--gc-sections -s -Wl,-rpath,\$ORIGIN/lib -Wl,-rpath,/home/linuxbrew/.linuxbrew/Cellar/ffmpeg/7.1.1_1/lib

ifneq ($(or $(filter Windows_NT,$(OS)),$(CROSS)),)
  EXE     := PurePlayer.exe
  RES     := app_res.o
  CFLAGS  += -mwindows -static-libgcc -I$(FFMPEG)/include -I$(SDL)/include
  LIBS    := $(FFMPEG)/lib/avformat.lib $(FFMPEG)/lib/avcodec.lib \
             $(FFMPEG)/lib/swscale.lib $(FFMPEG)/lib/swresample.lib \
             $(FFMPEG)/lib/avutil.lib $(SDL)/lib/libSDL3.a \
             -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion \
             -luuid -ladvapi32 -lsetupapi -lshell32 -lcfgmgr32 -lcomdlg32
else
  EXE     := PurePlayer
  RES     :=
  PKFF    := libavformat libavcodec libswscale libswresample libavutil
  CFLAGS  += $(shell pkg-config --cflags $(PKFF) sdl3)
  LIBS    := -L/home/linuxbrew/.linuxbrew/Cellar/ffmpeg/7.1.1_1/lib -lavformat -lavcodec -lswscale -lswresample -lavutil -lm -lpthread -L/home/rilex/Downloads/PurePlayer-1.0/sdl3-prefix/lib -lSDL3
endif

all: $(EXE)

app_res.o: assets/app.rc assets/video.ico assets/music.ico
	$(WINDRES) assets/app.rc -O coff -o $@

$(EXE): $(SRC) $(HDR) $(RES)
	$(CC) $(CFLAGS) $(SRC) $(RES) -o $@ $(LDFLAGS) $(LIBS)

.PHONY: all clean
clean:
	rm -f PurePlayer PurePlayer.exe app_res.o
