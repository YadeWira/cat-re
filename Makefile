# libcat + cat-tool Makefile

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g -std=c11 -fPIC -D_DEFAULT_SOURCE
INCLUDES = -Ilibcat -I$(OPENJPEG_INC)
LDFLAGS = -L$(OPENJPEG_LIB) -Wl,-rpath,$(OPENJPEG_LIB)
LDLIBS  = -lz
OPENJP2_LIB = /usr/lib/x86_64-linux-gnu/libopenjp2.so.7
JPEG_LIB = -ljpeg

LIB_SRCS = libcat/format.c libcat/dispatch.c \
           libcat/backends/raw.c libcat/backends/zip.c \
           libcat/backends/jp2.c libcat/backends/ole2.c \
           libcat/backends/jpg.c
LIB_OBJS = $(LIB_SRCS:.c=.o)
LIB_SO   = libcat/libcat.so

TOOL_SRC = tools/cat-tool.c
TOOL_BIN = cat-tool

TEST_SRCS = tests/c/test_format.c tests/c/test_zip.c tests/c/test_jp2.c tests/c/runtests_main.c
TEST_BIN  = tests/runtests

# Optional JPG corpus runners (built on demand: `make jpg-tests`)
JPG_FAST_SRC = tests/c/test_jpg_fast.c
JPG_RUN_SRC  = tests/c/test_jpg_runner.c

.PHONY: all test clean
all: catre $(LIB_SO) $(TOOL_BIN)

# CAT RE v1.0 — native C archiver (QCM single/multi/folders, deflate + JPEG2000).
# Image codec via OpenJPEG (static) + zlib. OPENJPEG_INC/LIB can be overridden.
OPENJPEG_INC ?= /tmp/openjpeg/extracted/usr/include/openjpeg-2.5
OPJ_STATIC   ?= /tmp/openjpeg/extracted/usr/lib/x86_64-linux-gnu/libopenjp2.a
CATRE_SRC = tools/catre.c tools/catre_img.c
CATRE_CFLAGS = -O2 -Wall -std=c11 -D_DEFAULT_SOURCE -I$(OPENJPEG_INC)

catre: $(CATRE_SRC)
	$(CC) $(CATRE_CFLAGS) -o catre $(CATRE_SRC) $(OPJ_STATIC) -lz -lm

# Fully static, zero-runtime-dependency binary (OpenJPEG + zlib + libc linked in).
.PHONY: catre-static
catre-static: $(CATRE_SRC)
	$(CC) $(CATRE_CFLAGS) -static -o catre-static $(CATRE_SRC) $(OPJ_STATIC) -l:libz.a -lm
	strip catre-static
	@echo "built catre-static ($$(stat -c%s catre-static) bytes); ldd:"; ldd catre-static 2>&1 | head -1

# ---- Windows cross-builds (mingw-w64) ----
# Static .exe for Win7+ (x64 and x86). Deps built by scripts/build-win-deps.sh.
WIN_DEPS   ?= /tmp/winbuild
WIN64_DEPS ?= $(WIN_DEPS)/out-x64
WIN32_DEPS ?= $(WIN_DEPS)/out-x86
WIN_CFLAGS  = -O2 -Wall -std=c11 -D_DEFAULT_SOURCE -DOPJ_STATIC

.PHONY: catre-win64 catre-win32
catre-win64: $(CATRE_SRC)
	x86_64-w64-mingw32-gcc $(WIN_CFLAGS) \
	  -I$(WIN64_DEPS)/include/openjpeg-2.5 -I$(WIN64_DEPS)/include \
	  -static -o catre-x64.exe $(CATRE_SRC) \
	  $(WIN64_DEPS)/lib/libopenjp2.a -L$(WIN64_DEPS)/lib -l:libz.a -lm
	x86_64-w64-mingw32-strip catre-x64.exe
	@echo "built catre-x64.exe ($$(stat -c%s catre-x64.exe) bytes)"

catre-win32: $(CATRE_SRC)
	i686-w64-mingw32-gcc $(WIN_CFLAGS) \
	  -I$(WIN32_DEPS)/include/openjpeg-2.5 -I$(WIN32_DEPS)/include \
	  -static -o catre-x86.exe $(CATRE_SRC) \
	  $(WIN32_DEPS)/lib/libopenjp2.a -L$(WIN32_DEPS)/lib -l:libz.a -lm
	i686-w64-mingw32-strip catre-x86.exe
	@echo "built catre-x86.exe ($$(stat -c%s catre-x86.exe) bytes)"

# All three release binaries into dist/.
.PHONY: dist
dist: catre-static catre-win64 catre-win32
	mkdir -p dist
	cp catre-static dist/catre-linux-x64
	cp catre-x64.exe dist/catre-windows-x64.exe
	cp catre-x86.exe dist/catre-windows-x86.exe
	@echo "=== dist/ ==="; ls -la dist/

$(LIB_SO): $(LIB_OBJS)
	$(CC) -shared -o $@ $^ -lz $(OPENJP2_LIB) $(JPEG_LIB) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(TOOL_BIN): $(TOOL_SRC) $(LIB_SO)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< -Llibcat -lcat $(LDFLAGS) $(LDLIBS) $(OPENJP2_LIB) $(JPEG_LIB)

$(TEST_BIN): $(TEST_SRCS) $(LIB_SO)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(TEST_SRCS) -Llibcat -lcat $(LDFLAGS) $(LDLIBS) $(OPENJP2_LIB) $(JPEG_LIB) -lm

test: $(TEST_BIN)
	LD_LIBRARY_PATH=libcat:$$LD_LIBRARY_PATH ./$(TEST_BIN)

.PHONY: jpg-tests
jpg-tests: $(LIB_SO)
	$(CC) $(CFLAGS) $(INCLUDES) -o tests/test_jpg_fast $(JPG_FAST_SRC) -Llibcat -lcat $(LDFLAGS) $(LDLIBS) $(OPENJP2_LIB) $(JPEG_LIB) -lm
	$(CC) $(CFLAGS) $(INCLUDES) -o tests/test_jpg_runner $(JPG_RUN_SRC) -Llibcat -lcat $(LDFLAGS) $(LDLIBS) $(OPENJP2_LIB) $(JPEG_LIB) -lm

clean:
	rm -f $(LIB_OBJS) $(LIB_SO) $(TOOL_BIN) $(TEST_BIN) catre tests/test_jpg_fast tests/test_jpg_runner
