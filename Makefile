# Makefile — arlink_stream (AArch64 cross-compiled)
#
# Downloads Linaro GCC 7.5 toolchain on first use, then builds.
# Run:   make
# Clean: make clean
# Full clean (removes toolchain too): make distclean

TOOLCHAIN_URL  := https://releases.linaro.org/components/toolchain/binaries/7.5-2019.12/aarch64-linux-gnu/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu.tar.xz
TOOLCHAIN_ARCH := gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu.tar.xz
TOOLCHAIN_DIR  := toolchain
TOOLCHAIN_BIN  := $(TOOLCHAIN_DIR)/gcc-linaro-7.5.0-2019.12-x86_64_aarch64-linux-gnu/bin

CC      := $(TOOLCHAIN_BIN)/aarch64-linux-gnu-gcc
STRIP   := $(TOOLCHAIN_BIN)/aarch64-linux-gnu-strip

TARGET  := arlink_stream

SRCS    := main.c pipeline.c rtp_h265.c udp_sender.c http_api.c
OBJS    := $(SRCS:.c=.o)

CFLAGS  := -O2 -Wall -Wextra -std=c99 \
           -march=armv8-a \
           -ffunction-sections -fdata-sections \
           -D_GNU_SOURCE

LDFLAGS := -Wl,--gc-sections \
           -Wl,-E \
           -ldl -lpthread -lrt

# ------------------------------------------------------------------ #

.PHONY: all clean distclean toolchain

all: toolchain $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	$(STRIP) $@

%.o: %.c | toolchain
	$(CC) $(CFLAGS) -c -o $@ $<

# ------------------------------------------------------------------ #
# Toolchain bootstrap

toolchain: $(CC)

$(CC): $(TOOLCHAIN_DIR)/$(TOOLCHAIN_ARCH)
	@echo "[toolchain] extracting..."
	tar -xf $(TOOLCHAIN_DIR)/$(TOOLCHAIN_ARCH) -C $(TOOLCHAIN_DIR)
	@touch $@

$(TOOLCHAIN_DIR)/$(TOOLCHAIN_ARCH):
	@echo "[toolchain] downloading Linaro GCC 7.5 for aarch64..."
	@mkdir -p $(TOOLCHAIN_DIR)
	wget -q --show-progress -O $@ $(TOOLCHAIN_URL)

# ------------------------------------------------------------------ #

clean:
	rm -f $(OBJS) $(TARGET)

distclean: clean
	rm -rf $(TOOLCHAIN_DIR)
