# language: Makefile, file: android/gles_stream/Makefile, target: Android NDK
# Build libGLES_x11droid.so (GLES 2.0 client that streams commands to the host GLX server).

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

NDK        ?= $(ANDROID_NDK_HOME)
API        ?= 30
TARGET_ARCH ?= x86_64
HOST_TAG   ?= linux-x86_64

ifeq ($(TARGET_ARCH),x86_64)
TRIPLE := x86_64-linux-android
else ifeq ($(TARGET_ARCH),arm64)
TRIPLE := aarch64-linux-android
else ifeq ($(TARGET_ARCH),arm)
TRIPLE := armv7a-linux-androideabi
else ifeq ($(TARGET_ARCH),x86)
TRIPLE := i686-linux-android
else
$(error unknown TARGET_ARCH: $(TARGET_ARCH) (x86_64/arm64/arm/x86))
endif

CC := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)/bin/$(TRIPLE)$(API)-clang

OUT_DIR := $(ROOT)/build/android/gles_stream
TARGET  := $(OUT_DIR)/libGLES_x11droid.so

SRCS := src/gles_client.c
HDRS := include/gles_stream_proto.h

CFLAGS := -std=c11 -fPIC -O2 -g -Wall -Wextra -Wshadow -Wformat=2 \
          -fno-strict-aliasing -Iinclude
LDFLAGS := -shared -Wl,-soname,libGLES_x11droid.so -Wl,--no-undefined
LDLIBS  := -llog -ldl

DESTDIR       ?= $(ROOT)/build/vendor-root
VENDOR_LIBDIR ?= lib64

.PHONY: all clean install

all: $(TARGET)

$(OUT_DIR):
	@mkdir -p $@

$(TARGET): $(SRCS) $(HDRS) | $(OUT_DIR)
	@test -n "$(NDK)" || { echo "ANDROID_NDK_HOME is not set" >&2; exit 1; }
	@test -x "$(CC)" || { echo "NDK compiler not found: $(CC)" >&2; exit 1; }
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LDLIBS)

install: $(TARGET)
	install -D -m 0644 $(TARGET) $(DESTDIR)/$(VENDOR_LIBDIR)/libGLES_x11droid.so

clean:
	rm -rf $(OUT_DIR) libs obj
