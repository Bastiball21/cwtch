# ===========================================================================
# Cwtch Makefile
# ===========================================================================

VERSION   := 7.1

SRC_DIR   := src
BUILD_DIR := build

# Auto-detect host OS and CPU architecture
ifeq ($(OS),Windows_NT)
  HOST_OS   := windows
  HOST_ARCH := x86_64
  TARGET    := ./cwtch.exe
else
  UNAME_S := $(shell uname -s 2>/dev/null || echo Unknown)
  UNAME_M := $(shell uname -m 2>/dev/null || echo x86_64)
  ifeq ($(UNAME_S),Linux)
    HOST_OS := linux
  else ifeq ($(UNAME_S),Darwin)
    HOST_OS := macos
  else
    HOST_OS := unix
  endif
  HOST_ARCH := $(UNAME_M)
  TARGET    := ./cwtch
endif

ifeq ($(HOST_ARCH),x86_64)
  ARCH_FLAG := -march=x86-64
else ifeq ($(HOST_ARCH),amd64)
  ARCH_FLAG := -march=x86-64
else ifeq ($(HOST_ARCH),aarch64)
  ARCH_FLAG := -march=armv8-a
else ifeq ($(HOST_ARCH),arm64)
  ARCH_FLAG := -march=armv8-a
else
  ARCH_FLAG := -march=native
endif

CC        := clang
BASE_CFLAGS := -Wall -Wextra -O3 -flto $(ARCH_FLAG) -funroll-loops -fno-math-errno -DBUILD=\"$(VERSION)\"
CFLAGS    := $(BASE_CFLAGS) -MMD -MP
LDFLAGS   := -flto -lm -lpthread -lzstd

# Linux release binaries are fully static, so they run without libzstd installed.
RELEASE_LDFLAGS := -flto -static -lm -lpthread -lzstd

# ---------------------------------------------------------------------------
# Windows cross-compile
# ---------------------------------------------------------------------------
# Requires:
#   sudo apt install gcc-mingw-w64-x86-64
# And a MinGW-compatible libzstd built by `make zstd-win` (see below).
#
# The zstd static lib shipped with the official Windows release is MSVC
# format (libzstd_static.lib) and cannot be linked with the MinGW toolchain.
# `make zstd-win` builds a proper libzstd.a into $(ZSTD_WIN_DIR).
# ---------------------------------------------------------------------------

WIN_CC     := clang --target=x86_64-w64-mingw32 -fuse-ld=lld
WIN_TARGET := ./cwtch.exe
WIN_BUILD_DIR := build-win

ZSTD_WIN_DIR     := $(HOME)/zstd-mingw
ZSTD_WIN_SRC_DIR := $(HOME)/zstd-src
ZSTD_WIN_VER     := v1.5.7

WIN_BASE_CFLAGS := -Wall -Wextra -O3 -flto -march=x86-64 -funroll-loops -fno-math-errno \
                   -DBUILD=\"$(VERSION)\" -I$(ZSTD_WIN_DIR)/include
WIN_CFLAGS  := $(WIN_BASE_CFLAGS) -MMD -MP

# -static links everything statically (self-contained .exe).
# If the toolchain complains about missing static libgcc, fall back to:
#   -static-libgcc -static-libstdc++ instead of -static
WIN_LDFLAGS := -flto -lm -lpthread -static \
               -Wl,--stack,8388608 \
               -L$(ZSTD_WIN_DIR)/lib -lzstd

# Debug
DEBUG_CFLAGS   := -Wall -Wextra -O1 -g -DBUILD=\"$(VERSION)\"
DEBUG_LDFLAGS  := -lm -lpthread -lzstd

# Sources
SRCS      := $(wildcard $(SRC_DIR)/*.c)
OBJS      := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
WIN_OBJS  := $(patsubst $(SRC_DIR)/%.c,$(WIN_BUILD_DIR)/%.o,$(SRCS))
DEPS      := $(OBJS:.o=.d)
WIN_DEPS  := $(WIN_OBJS:.o=.d)

# Release
RELEASE_DIR  := releases
ARM64_ARCHES := aarch64
ARM32_ARCHES := armv6 armv7_neon

ARM_CC_32 := arm-none-linux-gnueabihf-gcc
ARM_CC_64 := aarch64-linux-gnu-gcc

ARM32_ZSTD_LIB_DIR := /opt/arm-zstd/lib
ARM32_ZSTD_INC_DIR := /opt/arm-zstd/include

# ===========================================================================
# Phony targets
# ===========================================================================
.PHONY: all win universal universal-win clean rebuild debug test \
        release release-win release-aarch64 release-arm32 \
        release-all release-everything zstd-win

# ===========================================================================
# Universal build for this host: on x86-64 one binary dispatches its SIMD at runtime
# ===========================================================================
universal:
	@echo "  CC      $(HOST_OS) $(HOST_ARCH) ($(ARCH_FLAG)) -> $(TARGET)"
	@$(CC) $(BASE_CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

universal-win:
	@echo "  CC      universal x86-64 Windows (runtime dispatch: avx512_vnni | avx512bw | avx2 | scalar) -> $(WIN_TARGET)"
	@$(WIN_CC) $(WIN_BASE_CFLAGS) $(SRCS) -o $(WIN_TARGET) $(WIN_LDFLAGS)

# Perft, SIMD agreement, bench determinism and UCI behaviour
test: universal
	@bin/check $(TARGET)

# ===========================================================================
# Default build
# ===========================================================================
all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "  LD      $@ (OS=$(HOST_OS) ARCH=$(HOST_ARCH))"
	@$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# incbin isn't visible to -MMD: a new net must rebuild this object
$(BUILD_DIR)/net.o: quantised.bin

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ===========================================================================
# Windows cross-compile
# ===========================================================================
win: $(WIN_TARGET)

$(WIN_TARGET): $(WIN_OBJS)
	@echo "  LD      $@"
	@$(WIN_CC) $(WIN_OBJS) -o $@ $(WIN_LDFLAGS)

$(WIN_BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(WIN_BUILD_DIR)
	@echo "  CC      $< (win)"
	@$(WIN_CC) $(WIN_CFLAGS) -c $< -o $@

$(WIN_BUILD_DIR)/net.o: quantised.bin

$(WIN_BUILD_DIR):
	@mkdir -p $(WIN_BUILD_DIR)

# ===========================================================================
# zstd-win: build a MinGW-compatible libzstd.a for cross-compilation
# ===========================================================================
# Downloads zstd source, cross-compiles it for x86_64-w64-mingw32, and
# installs the resulting libzstd.a + headers into $(ZSTD_WIN_DIR).
zstd-win:
	@if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then \
		echo "  error: x86_64-w64-mingw32-gcc not found." ; \
		echo "  Install: sudo apt install gcc-mingw-w64-x86-64" ; \
		exit 1 ; \
	fi
	@if [ -f "$(ZSTD_WIN_DIR)/lib/libzstd.a" ]; then \
		echo "  zstd-win already built at $(ZSTD_WIN_DIR)/lib/libzstd.a" ; \
		echo "  (delete it to rebuild)" ; \
		exit 0 ; \
	fi
	@if [ ! -d "$(ZSTD_WIN_SRC_DIR)" ]; then \
		echo "  === Fetching zstd $(ZSTD_WIN_VER) ===" ; \
		git clone --depth 1 --branch $(ZSTD_WIN_VER) \
			https://github.com/facebook/zstd.git $(ZSTD_WIN_SRC_DIR) ; \
	fi
	@echo "  === Building libzstd.a for MinGW ==="
	@cd $(ZSTD_WIN_SRC_DIR) && \
		$(MAKE) -C lib lib-release \
			CC=x86_64-w64-mingw32-gcc \
			AR=x86_64-w64-mingw32-ar \
			CFLAGS="-O3 -fPIC" -j$$(nproc)
	@mkdir -p $(ZSTD_WIN_DIR)/lib $(ZSTD_WIN_DIR)/include
	@cp $(ZSTD_WIN_SRC_DIR)/lib/libzstd.a $(ZSTD_WIN_DIR)/lib/
	@cp $(ZSTD_WIN_SRC_DIR)/lib/zstd.h \
		$(ZSTD_WIN_SRC_DIR)/lib/zstd_errors.h \
		$(ZSTD_WIN_SRC_DIR)/lib/zdict.h \
		$(ZSTD_WIN_DIR)/include/ 2>/dev/null || true
	@echo "  === Installed $(ZSTD_WIN_DIR)/lib/libzstd.a ==="

# ===========================================================================
# Debug
# ===========================================================================
debug:
	@mkdir -p $(BUILD_DIR)
	@echo "  CC      debug build -> $(TARGET)"
	@$(CC) $(DEBUG_CFLAGS) $(SRCS) -o $(TARGET) $(DEBUG_LDFLAGS)

# ===========================================================================
# Clean
# ===========================================================================
clean:
	@rm -rf $(BUILD_DIR) $(WIN_BUILD_DIR) build-arm build-arm64
	@rm -f $(TARGET) $(WIN_TARGET) ./cwtch_datagen
	@echo "  CLEAN   done (releases/ left alone)"

rebuild: clean all

# ===========================================================================
# Release: x86_64 (Linux - Universal runtime SIMD dispatch)
# ===========================================================================
release:
	@mkdir -p $(RELEASE_DIR)
	@out="$(RELEASE_DIR)/cwtch_$(VERSION)_linux_x86_64" ; \
	echo "=== Linux Universal x86_64 (runtime dispatch: avx512_vnni | avx512bw | avx2 | scalar) -> $$out ===" ; \
	$(CC) $(BASE_CFLAGS) $(SRCS) -o $$out $(RELEASE_LDFLAGS)
	@echo "=== x86_64 Linux release complete ==="

# ===========================================================================
# Release: x86_64 (Windows - Universal runtime SIMD dispatch)
# ===========================================================================
release-win:
	@mkdir -p $(RELEASE_DIR)
	@if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then \
		echo "  error: x86_64-w64-mingw32-gcc not found." ; \
		echo "  Install: sudo apt install gcc-mingw-w64-x86-64" ; \
		exit 1 ; \
	fi
	@if [ ! -f "$(ZSTD_WIN_DIR)/lib/libzstd.a" ]; then \
		echo "  error: $(ZSTD_WIN_DIR)/lib/libzstd.a not found." ; \
		echo "  Run: make zstd-win" ; \
		exit 1 ; \
	fi
	@if ! echo '#include <zstd.h>' | $(WIN_CC) $(WIN_BASE_CFLAGS) -E -x c - >/dev/null 2>&1; then \
		echo "  error: $(WIN_CC) cannot find zstd.h" ; \
		echo "  Looked in: $(ZSTD_WIN_DIR)/include" ; \
		exit 1 ; \
	fi
	@out="$(RELEASE_DIR)/cwtch_$(VERSION)_windows_x86_64.exe" ; \
	echo "=== Windows Universal x86_64 (runtime dispatch: avx512_vnni | avx512bw | avx2 | scalar) -> $$out ===" ; \
	$(WIN_CC) $(WIN_BASE_CFLAGS) $(SRCS) -o $$out $(WIN_LDFLAGS)
	@echo "=== x86_64 Windows release complete ==="

# ===========================================================================
# Release: aarch64 (Linux)
# ===========================================================================
release-aarch64:
	@mkdir -p $(RELEASE_DIR)
	@if ! command -v $(ARM_CC_64) >/dev/null 2>&1; then \
		echo "  error: $(ARM_CC_64) not found." ; \
		echo "  Install: sudo apt install gcc-aarch64-linux-gnu libc6-dev-arm64-cross libzstd-dev:arm64" ; \
		exit 1 ; \
	fi
	@for arch in $(ARM64_ARCHES); do \
		out="$(RELEASE_DIR)/cwtch_$(VERSION)_$$arch" ; \
		echo "=== aarch64 -> $$out ===" ; \
		$(ARM_CC_64) -Wall -Wextra -O3 -flto -march=armv8-a -DBUILD=\"$(VERSION)\" \
			$(SRCS) -o $$out $(RELEASE_LDFLAGS) || exit 1 ; \
	done
	@echo "=== aarch64 release complete ==="

# ===========================================================================
# Release: ARM32 (Linux, external toolchain)
# ===========================================================================
release-arm32:
	@mkdir -p $(RELEASE_DIR)
	@if ! command -v $(ARM_CC_32) >/dev/null 2>&1; then \
		echo "  error: $(ARM_CC_32) not found." ; \
		echo "  Install the ARM GNU Toolchain: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads" ; \
		exit 1 ; \
	fi
	@if [ ! -f $(ARM32_ZSTD_LIB_DIR)/libzstd.a ]; then \
		echo "  error: $(ARM32_ZSTD_LIB_DIR)/libzstd.a not found." ; \
		echo "  Build libzstd for ARM first, or disable zstd for ARM builds." ; \
		exit 1 ; \
	fi
	@for arch in $(ARM32_ARCHES); do \
		case $$arch in \
			armv6)      march=armv6;   mfpu=vfp ;; \
			armv7_neon) march=armv7-a; mfpu=neon-vfpv4 ;; \
			*) echo "Unknown ARM32 arch: $$arch" ; exit 1 ;; \
		esac ; \
		out="$(RELEASE_DIR)/cwtch_$(VERSION)_$$arch" ; \
		echo "=== ARM32: $$arch -> $$out ===" ; \
		$(ARM_CC_32) -Wall -Wextra -O3 -flto -march=$$march -mfpu=$$mfpu \
			-I$(ARM32_ZSTD_INC_DIR) \
			-DBUILD=\"$(VERSION)\" $(SRCS) \
			-L$(ARM32_ZSTD_LIB_DIR) \
			-o $$out \
			-flto -lm -lpthread -lzstd -static || exit 1 ; \
	done
	@echo "=== ARM32 release complete ==="

# ===========================================================================
# Convenience
# ===========================================================================
release-all: release release-win release-aarch64
	@echo "=== Full release build complete ==="

release-everything: release release-win release-aarch64 release-arm32
	@echo "=== Everything built ==="

-include $(DEPS)
-include $(WIN_DEPS)