# language: GNU Make, file: Makefile, target: Linux x86_64, gcc/g++ (no CMake)
# x11droid top-level Makefile. Each component owns its Makefile; the tree can grow incrementally.

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

# Vendor overlay configuration (unified from DESTDIR + VENDOR_LIBDIR + HWC_NAME)
VENDOR_DIR      ?= $(BUILD_DIR)/vendor-root
VENDOR_HW_DIR   := $(VENDOR_DIR)/lib64/hw
VENDOR_LIB_DIR  := $(VENDOR_DIR)/lib64

# Android build configuration
ANDROID_NDK_HOME ?=
ANDROID_API     ?= 30
ANDROID_ARCH    ?= x86_64
HWC_NAME        ?= x11

# Find NDK if not set
ifndef ANDROID_NDK_HOME
  NDK_CANDIDATES := \
    $(HOME)/Android/Sdk/ndk \
    /opt/android-ndk \
    /usr/local/android-ndk \
    /usr/lib/android-ndk
  ANDROID_NDK_HOME := $(firstword $(wildcard $(addsuffix /*,$(NDK_CANDIDATES))))
endif

export CC CXX BUILD_DIR CFLAGS_COMMON CXXFLAGS_COMMON LDFLAGS_COMMON \
       ANDROID_NDK_HOME ANDROID_API ANDROID_ARCH HWC_NAME \
       VENDOR_DIR VENDOR_HW_DIR VENDOR_LIB_DIR

HOST_COMPONENTS    := host-daemon host/common host/input
ANDROID_COMPONENTS := android/hwcomposer android/gles_stream
COMPONENTS         := $(HOST_COMPONENTS) $(ANDROID_COMPONENTS)

# Components without a Makefile are skipped instead of failing the top-level build.
BUILDABLE := $(patsubst %/Makefile,%,$(wildcard $(addsuffix /Makefile,$(COMPONENTS))))
PENDING   := $(filter-out $(BUILDABLE),$(COMPONENTS))

# Only print this notice for build targets, not for help/status/clean.
ifneq ($(filter all host android setup,$(or $(MAKECMDGOALS),all)),)
ifneq ($(PENDING),)
$(info [x11droid] missing Makefile: $(PENDING) — skipped)
endif
endif

.PHONY: all host android setup start stop status doctor check-deps run demo-producer clean distclean help $(COMPONENTS)

all: host android

host: $(filter $(HOST_COMPONENTS),$(BUILDABLE))

android:
	@if [ -z "$(ANDROID_NDK_HOME)" ]; then \
	  echo "[android] ANDROID_NDK_HOME not found. Searched:"; \
	  for p in $(NDK_CANDIDATES); do echo "  $$p"; done; \
	  echo "Set ANDROID_NDK_HOME or install NDK r23+ (e.g. ~/Android/Sdk/ndk/26.x)"; \
	else \
	  for d in $(ANDROID_COMPONENTS); do \
	    if [ -f "$$d/Makefile" ]; then $(MAKE) -C "$$d" all || exit 1; fi; \
	  done; \
	fi

$(BUILDABLE):
	+@$(MAKE) -C $@ all

# --- New unified targets ---

setup:
	@bash scripts/check-deps.sh $(DEPS_FLAGS) || true
	@$(MAKE) host
	@$(MAKE) android
	@echo "[setup] Installing to vendor overlay: $(VENDOR_DIR)"
	@$(MAKE) -C android/hwcomposer install DESTDIR=$(VENDOR_DIR) VENDOR_LIBDIR=lib64 HWC_NAME=$(HWC_NAME)
	@$(MAKE) -C android/gles_stream install DESTDIR=$(VENDOR_DIR) VENDOR_LIBDIR=lib64
	@echo "[setup] Writing build.prop"
	@mkdir -p $(VENDOR_DIR)
	@echo "ro.hardware.hwcomposer=$(HWC_NAME)" > $(VENDOR_DIR)/build.prop
	@echo "[setup] Setting permissions"
	@chmod 0644 $(VENDOR_HW_DIR)/hwcomposer.$(HWC_NAME).so 2>/dev/null || true
	@chown 0:0 $(VENDOR_HW_DIR)/hwcomposer.$(HWC_NAME).so 2>/dev/null || true
	@command -v chcon >/dev/null 2>&1 && chcon u:object_r:vendor_file:s0 $(VENDOR_HW_DIR)/hwcomposer.$(HWC_NAME).so 2>/dev/null || true
	@echo "[setup] Done. Vendor overlay ready at $(VENDOR_DIR)"

start:
	@$(MAKE) run-daemon
	@$(MAKE) run-container

run-daemon: host
	@bin="$(BUILD_DIR)/host-daemon/x11droid-daemon"; \
	if [ ! -x "$$bin" ]; then echo "[start] daemon not built — run: make host" >&2; exit 1; fi; \
	if [ -S /tmp/x11droid.sock ] && [ -f /dev/shm/x11droid_fb ]; then \
	  echo "[start] daemon already running"; \
	else \
	  echo "[start] starting daemon on $$DISPLAY"; \
	  exec "$$bin" --socket /tmp/x11droid.sock; \
	fi

run-container:
	@if [ "$$(id -u)" -ne 0 ]; then \
	  echo "[start] container needs root: sudo make start"; exit 1; \
	fi
	@if [ ! -S /tmp/x11droid.sock ] || [ ! -f /dev/shm/x11droid_fb ]; then \
	  echo "[start] daemon not ready — start it first (make run-daemon)"; exit 1; \
	fi
	@if [ ! -f container/images/system.img ] || [ ! -f container/images/vendor.img ]; then \
	  echo "[start] images missing: place system.img + vendor.img in container/images/"; exit 1; \
	fi
	@scripts/launch_container.sh --images container/images

stop:
	@if [ "$$(id -u)" -ne 0 ]; then \
	  echo "[stop] needs root: sudo make stop"; exit 1; \
	fi
	@scripts/launch_container.sh --stop
	@pkill -x x11droid-daemon 2>/dev/null || true
	@echo "[stop] done"

status:
	@printf '%-16s %s\n' "component" "makefile"
	@for d in $(COMPONENTS); do \
	  if [ -f "$$d/Makefile" ]; then printf '%-16s %s\n' "$$d" "present"; \
	  else printf '%-16s %s\n' "$$d" "missing"; fi; \
	done
	@echo ""
	@printf '%-20s %s\n' "artifact" "status"
	@for f in \
	  "$(BUILD_DIR)/host-daemon/x11droid-daemon" \
	  "$(BUILD_DIR)/android/hwcomposer/hwcomposer.$(HWC_NAME).so" \
	  "$(BUILD_DIR)/android/gles_stream/libGLES_x11droid.so" \
	  "$(VENDOR_HW_DIR)/hwcomposer.$(HWC_NAME).so" \
	  "$(VENDOR_DIR)/lib64/libGLES_x11droid.so" \
	  "$(VENDOR_DIR)/build.prop"; do \
	  if [ -f "$$f" ]; then printf '%-20s %s\n' "$$f" "OK"; \
	  else printf '%-20s %s\n' "$$f" "MISSING"; fi; \
	done
	@echo ""
	@if [ -S /tmp/x11droid.sock ]; then echo "daemon socket: OK"; else echo "daemon socket: MISSING"; fi
	@if [ -f /dev/shm/x11droid_fb ]; then echo "framebuffer:    OK"; else echo "framebuffer:    MISSING"; fi
	@if [ -c /dev/uinput ]; then echo "uinput device:  OK"; else echo "uinput device:  MISSING"; fi

doctor:
	@echo "=== x11droid doctor ==="
	@echo ""
	@echo "--- Build prerequisites ---"
	@bash scripts/check-deps.sh 2>&1 | grep -E '(OK|FAIL|WARN)' || true
	@echo ""
	@echo "--- NDK ---"
	@if [ -n "$(ANDROID_NDK_HOME)" ] && [ -d "$(ANDROID_NDK_HOME)" ]; then \
	  echo "ANDROID_NDK_HOME=$(ANDROID_NDK_HOME) [OK]"; \
	  $(ANDROID_NDK_HOME)/toolchains/llvm/prebuilt/linux-x86_64/bin/clang --version | head -1; \
	else \
	  echo "ANDROID_NDK_HOME not set or invalid"; \
	  echo "  Fix: export ANDROID_NDK_HOME=/path/to/ndk"; \
	fi
	@echo ""
	@echo "--- Runtime ---"
	@if [ -n "$$DISPLAY" ]; then echo "DISPLAY=$$DISPLAY [OK]"; else echo "DISPLAY not set [FAIL]"; echo "  Fix: run inside Xorg session or export DISPLAY=:0"; fi
	@if [ -S /tmp/x11droid.sock ]; then echo "daemon socket: OK"; else echo "daemon socket: MISSING"; echo "  Fix: make run-daemon"; fi
	@if [ -f /dev/shm/x11droid_fb ]; then echo "framebuffer: OK"; else echo "framebuffer: MISSING"; echo "  Fix: make run-daemon"; fi
	@if [ -c /dev/uinput ]; then echo "uinput: OK"; else echo "uinput: MISSING"; echo "  Fix: sudo modprobe uinput; sudo usermod -aG input $$USER"; fi
	@if [ -f container/images/system.img ] && [ -f container/images/vendor.img ]; then \
	  echo "Android images: OK"; \
	else \
	  echo "Android images: MISSING"; \
	  echo "  Fix: place system.img + vendor.img in container/images/"; \
	fi
	@if [ "$$(id -u)" -eq 0 ]; then echo "root: OK"; else echo "root: MISSING (needed for container)"; echo "  Fix: run container commands with sudo"; fi
	@echo ""
	@echo "--- Artifacts ---"
	@$(MAKE) --no-print-directory status 2>/dev/null | tail -20

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

clean:
	@for d in $(COMPONENTS); do if [ -f "$$d/Makefile" ]; then $(MAKE) -C "$$d" clean; fi; done
	@rm -rf $(BUILD_DIR)
	@echo "[clean] build artifacts removed"

distclean: clean
	@rm -f container/images/*.img container/images/*.tar container/images/*.tar.*
	@echo "[distclean] container images removed"

help:
	@echo "x11droid — Android-on-Linux container bridge (NVIDIA 470 + GLX)"
	@echo ""
	@echo "Quick start:"
	@echo "  make setup   # check deps, build all, install to vendor overlay"
	@echo "  make start   # run daemon + container (needs root, images in container/images/)"
	@echo "  make stop    # stop container and daemon"
	@echo ""
	@echo "Build targets:"
	@echo "  make all           build host + android (needs NDK)"
	@echo "  make host          build host-daemon + test_producer"
	@echo "  make android       build hwcomposer + libGLES (needs NDK)"
	@echo "  make setup         full build + install to vendor overlay + build.prop"
	@echo ""
	@echo "Runtime targets:"
	@echo "  make start         run daemon + container (needs root, images)"
	@echo "  make run-daemon    run host daemon only (foreground)"
	@echo "  make run-container run container only (needs root, daemon running)"
	@echo "  make stop          stop container and daemon"
	@echo "  make status        show component + artifact + runtime status"
	@echo "  make doctor        diagnose all prerequisites with fixes"
	@echo "  make check-deps    check host dependencies (binderfs, uinput, LXC, X11, GLX)"
	@echo ""
	@echo "Utility targets:"
	@echo "  make run           run host daemon (alias for run-daemon)"
	@echo "  make demo-producer send test pattern to daemon window"
	@echo "  make dist          create x11droid-release.tar.gz"
	@echo "  make clean         remove build artifacts"
	@echo "  make distclean     clean + remove container images"
	@echo "  make help          show this help"
	@echo ""
	@echo "Variables:"
	@echo "  ANDROID_NDK_HOME   NDK root (auto-detected if unset)"
	@echo "  ANDROID_API        API level (default: 30)"
	@echo "  ANDROID_ARCH       x86_64 | arm64 | arm | x86 (default: x86_64)"
	@echo "  HWC_NAME           hwcomposer name (default: x11)"
	@echo "  VENDOR_DIR         vendor overlay root (default: build/vendor-root)"
	@echo "  DEPS_FLAGS         flags for check-deps (e.g. --strict)"

DIST_FILES := \
	$(BUILD_DIR)/host-daemon/x11droid-daemon \
	$(BUILD_DIR)/android/hwcomposer/hwcomposer.$(HWC_NAME).so \
	$(BUILD_DIR)/android/gles_stream/libGLES_x11droid.so \
	scripts/launch_container.sh \
	scripts/check-deps.sh \
	lxc-config/config \
	README.md \
	CONTRIBUTING.md \
	report.txt

dist: all
	@if [ ! -f $(BUILD_DIR)/host-daemon/x11droid-daemon ]; then echo "[dist] host-daemon is not built" >&2; exit 1; fi
	@if [ ! -f $(BUILD_DIR)/android/hwcomposer/hwcomposer.$(HWC_NAME).so ]; then echo "[dist] hwcomposer.$(HWC_NAME).so is not built (NDK required)" >&2; exit 1; fi
	@if [ ! -f $(BUILD_DIR)/android/gles_stream/libGLES_x11droid.so ]; then echo "[dist] libGLES_x11droid.so is not built (NDK required)" >&2; exit 1; fi
	@mkdir -p $(BUILD_DIR)/dist/x11droid
	@cp $(DIST_FILES) $(BUILD_DIR)/dist/x11droid/
	@cd $(BUILD_DIR)/dist && tar -czf ../x11droid-release.tar.gz x11droid
	@echo "[dist] archive created: $(BUILD_DIR)/x11droid-release.tar.gz"
	@ls -lh $(BUILD_DIR)/x11droid-release.tar.gz