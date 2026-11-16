# language: GNU Make, file: Makefile, target: Linux x86_64, gcc/g++ (no CMake)
# x11-droid top-level Makefile. Each component owns its Makefile; the tree can grow incrementally.

SHELL        := /bin/bash
MAKEFLAGS    += --no-print-directory
.DEFAULT_GOAL := all

CC     ?= gcc
CXX    ?= g++
BUILD_DIR ?= $(CURDIR)/build
PREFIX ?= /usr/local

CFLAGS_COMMON   := -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Wshadow -Wformat=2 -Iinclude
CXXFLAGS_COMMON := -std=c++17 -O2 -g -Wall -Wextra -Wshadow -Wformat=2 -Iinclude
LDFLAGS_COMMON  := -Wl,--as-needed

export CC CXX BUILD_DIR CFLAGS_COMMON CXXFLAGS_COMMON LDFLAGS_COMMON

HOST_COMPONENTS    := host-daemon host/common host/input
ANDROID_COMPONENTS := android/hwcomposer android/gles_stream
COMPONENTS         := $(HOST_COMPONENTS) $(ANDROID_COMPONENTS)

# Components without a Makefile are skipped instead of failing the top-level build.
BUILDABLE := $(patsubst %/Makefile,%,$(wildcard $(addsuffix /Makefile,$(COMPONENTS))))
PENDING   := $(filter-out $(BUILDABLE),$(COMPONENTS))

# Only print this notice for build targets, not for help/status/clean.
ifneq ($(filter all host android,$(or $(MAKECMDGOALS),all)),)
ifneq ($(PENDING),)
$(info [xorgdroid] missing Makefile: $(PENDING) — skipped)
endif
endif

.PHONY: all host android check-deps run demo-producer clean distclean help status dist $(COMPONENTS)

all: host android

host: $(filter $(HOST_COMPONENTS),$(BUILDABLE))
android:
	@if [ -z "$(ANDROID_NDK_HOME)" ]; then \
	  echo "[android] ANDROID_NDK_HOME is not set — skipping Android components"; \
	else \
	  for d in $(ANDROID_COMPONENTS); do \
	    if [ -f "$$d/Makefile" ]; then $(MAKE) -C "$$d" all || exit 1; fi; \
	  done; \
	fi

$(BUILDABLE):
	+@$(MAKE) -C $@ all

check-deps:
	@bash scripts/check-deps.sh $(DEPS_FLAGS)

run: host-daemon
	@bin="$(BUILD_DIR)/host-daemon/x11droid-daemon"; \
	if [ ! -x "$$bin" ]; then echo "[run] daemon is not built — run: make host" >&2; exit 1; fi; \
	echo "[run] $$bin"; exec "$$bin" --socket "$${XORG_DROID_SOCK:-/tmp/x11droid.sock}"

demo-producer: host-daemon
	@bin="$(BUILD_DIR)/host-daemon/test_producer"; \
	if [ ! -x "$$bin" ]; then echo "[demo] producer is not built" >&2; exit 1; fi; \
	echo "[demo] $$bin"; exec "$$bin" --socket "$${XORG_DROID_SOCK:-/tmp/x11droid.sock}"

status:
	@printf '%-16s %s\n' "component" "makefile"
	@for d in $(COMPONENTS); do \
	  if [ -f "$$d/Makefile" ]; then printf '%-16s %s\n' "$$d" "present"; \
	  else printf '%-16s %s\n' "$$d" "missing"; fi; \
	done

clean:
	@for d in $(COMPONENTS); do if [ -f "$$d/Makefile" ]; then $(MAKE) -C "$$d" clean; fi; done
	@rm -rf $(BUILD_DIR)
	@echo "[clean] build artifacts removed"

distclean: clean
	@rm -f container/images/*.img container/images/*.tar container/images/*.tar.*
	@echo "[distclean] container images removed"

help:
	@echo "x11-droid — X11 Android Container Bridge (NVIDIA 470)"
	@echo ""
	@echo "  make all            build every component that has a Makefile"
	@echo "  make host           host-daemon + host/common + host/input"
	@echo "  make android        android/hwcomposer + android/gles_stream (requires NDK)"
	@echo "  make check-deps     check host dependencies (binderfs, uinput, LXC, X11, GLX 470)"
	@echo "  make run            run the X11 host daemon (host-daemon/)"
	@echo "  make demo-producer  send a test color pattern to the window"
	@echo "  make status         list components and Makefile availability"
	@echo "  make clean          remove build artifacts"
	@echo "  make dist           create x11droid-release.tar.gz"
	@echo "  make help           show this help"

DIST_FILES := \
	$(BUILD_DIR)/host-daemon/x11droid-daemon \
	$(BUILD_DIR)/android/hwcomposer/hwcomposer.x11.so \
	$(BUILD_DIR)/android/gles_stream/libGLES_x11droid.so \
	scripts/launch_container.sh \
	scripts/check-deps.sh \
	lxc-config/config \
	README.md \
	CONTRIBUTING.md \
	report.txt

dist: all
	@if [ ! -f $(BUILD_DIR)/host-daemon/x11droid-daemon ]; then echo "[dist] host-daemon is not built" >&2; exit 1; fi
	@if [ ! -f $(BUILD_DIR)/android/hwcomposer/hwcomposer.x11.so ]; then echo "[dist] hwcomposer.x11.so is not built (NDK required)" >&2; exit 1; fi
	@if [ ! -f $(BUILD_DIR)/android/gles_stream/libGLES_x11droid.so ]; then echo "[dist] libGLES_x11droid.so is not built (NDK required)" >&2; exit 1; fi
	@mkdir -p $(BUILD_DIR)/dist/x11droid
	@cp $(DIST_FILES) $(BUILD_DIR)/dist/x11droid/
	@cd $(BUILD_DIR)/dist && tar -czf ../x11droid-release.tar.gz x11droid
	@echo "[dist] archive created: $(BUILD_DIR)/x11droid-release.tar.gz"
	@ls -lh $(BUILD_DIR)/x11droid-release.tar.gz
