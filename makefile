
# Version (used in release binary names)
VERSION := 6.4

# Configurable paths
SRC_DIR := src
BUILD_DIR := build

# Output binary
TARGET := ./cwtch

# Compiler settings
CC := clang
CFLAGS := -Wall -Wextra -O3 -flto -march=native -MMD -MP -DBUILD=\"$(VERSION)\"
LDFLAGS := -flto -lm -lpthread

# Windows cross-compile settings
WIN_CC := clang --target=x86_64-w64-mingw32 -fuse-ld=lld
WIN_TARGET := ./cwtch.exe
WIN_BUILD_DIR := build-win
WIN_CFLAGS := -Wall -Wextra -O3 -flto -march=native -MMD -MP -DBUILD=\"$(VERSION)\"
WIN_LDFLAGS := -flto -lm -lpthread -static -Wl,--stack,8388608 

# Debug settings (for valgrind/gdb)
DEBUG_CFLAGS := -Wall -Wextra -O1 -g -DBUILD=\"$(VERSION)\"
DEBUG_LDFLAGS := -lm -lpthread

# Find all .c files in source directory
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
WIN_OBJS := $(patsubst $(SRC_DIR)/%.c,$(WIN_BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)
WIN_DEPS := $(WIN_OBJS:.o=.d)

# Default target
all: $(TARGET)

# Link
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

# Compile
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Windows cross-compile
win: $(WIN_TARGET)

$(WIN_TARGET): $(WIN_OBJS)
	$(WIN_CC) $(WIN_OBJS) -o $@ $(WIN_LDFLAGS)

$(WIN_BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(WIN_BUILD_DIR)
	$(WIN_CC) $(WIN_CFLAGS) -c $< -o $@

$(WIN_BUILD_DIR):
	mkdir -p $(WIN_BUILD_DIR)

# Clean
clean:
	rm -rf $(BUILD_DIR) $(WIN_BUILD_DIR) $(TARGET) $(WIN_TARGET)
	#rm -f $(foreach arch,$(RELEASE_ARCHES),releases/cwtch_$(VERSION)_$(arch) releases/cwtch_$(VERSION)_$(arch).exe)

# Rebuild
rebuild: clean all

# Debug build (for valgrind/gdb)
debug: clean
	$(CC) $(DEBUG_CFLAGS) $(SRCS) -o $(TARGET) $(DEBUG_LDFLAGS)

# Release architectures
RELEASE_ARCHES := x86_64 x86_64_v3 x86_64_v4
RELEASE_DIR := releases

# Release build (all arches, Linux)
release:
	mkdir -p $(RELEASE_DIR)
	@for arch in $(RELEASE_ARCHES); do \
		echo "=== Building cwtch_$(VERSION)_$$arch ===" ; \
		march=$$(echo $$arch | sed 's/_/-/g') ; \
		$(CC) -Wall -Wextra -O3 -flto -march=$$march -DBUILD=\"$(VERSION)\" $(SRCS) -o $(RELEASE_DIR)/cwtch_$(VERSION)_$$arch $(LDFLAGS) ; \
		echo "  Done: $(RELEASE_DIR)/cwtch_$(VERSION)_$$arch" ; \
	done
	@echo "=== Release build complete ==="

# Release build (all arches, Windows)
release-win:
	mkdir -p $(RELEASE_DIR)
	@for arch in $(RELEASE_ARCHES); do \
		echo "=== Building cwtch_$(VERSION)_$$arch.exe ===" ; \
		march=$$(echo $$arch | sed 's/_/-/g') ; \
		$(WIN_CC) -Wall -Wextra -O3 -flto -march=$$march -DBUILD=\"$(VERSION)\" $(SRCS) -o $(RELEASE_DIR)/cwtch_$(VERSION)_$$arch.exe $(WIN_LDFLAGS) ; \
		echo "  Done: $(RELEASE_DIR)/cwtch_$(VERSION)_$$arch.exe" ; \
	done
	@echo "=== Release build complete ==="

-include $(DEPS)
-include $(WIN_DEPS)
