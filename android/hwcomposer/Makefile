# language: Makefile, file: android/hwcomposer/Makefile, target: Android NDK
# Build hwcomposer.x11.so. Requires ANDROID_NDK_HOME (NDK r23+).
# Examples:
#   ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/26.1.10909125 make
#   make TARGET_ARCH=arm64 API=30 HWC_NAME=default
#   make HWC_NAME=redroid     # match getprop ro.hardware

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

NDK        ?= $(ANDROID_NDK_HOME)
API        ?= 30
TARGET_ARCH ?= x86_64
HOST_TAG   ?= linux-x86_64
HWC_NAME   ?= x11

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

CXX     := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)/bin/$(TRIPLE)$(API)-clang++
HOST_CXX ?= g++

OUT_DIR := $(ROOT)/build/android/hwcomposer
TARGET  := $(OUT_DIR)/hwcomposer.$(HWC_NAME).so

SRCS := hwcomposer_x11.cpp xd_shm.cpp xd_gralloc.cpp
HDRS := xd_shm.h xd_gralloc.h xd_fb_layout.h xd_log.h

CXXFLAGS := -std=c++17 -fPIC -O2 -g -Wall -Wextra -Wno-unused-parameter \
            -fvisibility=hidden -fvisibility-inlines-hidden -pthread -Iinclude -I.
LDFLAGS  := -shared -pthread -static-libstdc++ \
            -Wl,-soname,hwcomposer.$(HWC_NAME).so \
            -Wl,--no-undefined -Wl,--exclude-libs,ALL
LDLIBS   := -llog -ldl

# Default staging directory; copy DESTDIR contents into the vendor overlay image.
DESTDIR       ?= $(ROOT)/build/vendor-root
VENDOR_LIBDIR ?= lib64

.PHONY: all check clean install

all: $(TARGET)

$(OUT_DIR):
	@mkdir -p $@

$(TARGET): $(SRCS) $(HDRS) | $(OUT_DIR)
	@test -n "$(NDK)" || { echo "ANDROID_NDK_HOME is not set" >&2; exit 1; }
	@test -x "$(CXX)" || { echo "NDK compiler not found: $(CXX)" >&2; exit 1; }
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LDLIBS)

# Fast host syntax check without the NDK.
check:
	$(HOST_CXX) -std=c++17 -fsyntax-only -Wall -Wextra \
		-Wno-unused-parameter -Iinclude -I. $(SRCS)

install: $(TARGET)
	install -D -m 0644 $(TARGET) \
		$(DESTDIR)/$(VENDOR_LIBDIR)/hw/hwcomposer.$(HWC_NAME).so

clean:
	rm -rf $(OUT_DIR)
