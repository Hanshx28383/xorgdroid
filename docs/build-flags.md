# Build Flags & Variables

## Top-Level (root Makefile)

| Variable | Default | Effect |
|----------|---------|--------|
| `CC` | `gcc` | Host C compiler |
| `CXX` | `g++` | Host C++ compiler |
| `BUILD_DIR` | `$(CURDIR)/build` | All build artifacts |
| `CFLAGS_COMMON` | C11, `-O2 -g`, warnings, `-Iinclude` | Exported to host components |
| `CXXFLAGS_COMMON` | C++17, `-O2 -g`, warnings, `-Iinclude` | Exported to host components |
| `LDFLAGS_COMMON` | `-Wl,--as-needed` | Exported to host components |
| `ANDROID_NDK_HOME` | auto-detected | Enables `android` target |
| `ANDROID_API` | `30` | Android API level |
| `ANDROID_ARCH` | `x86_64` | `x86_64`, `arm64`, `arm`, `x86` |
| `HWC_NAME` | `x11` | Output `hwcomposer.$(HWC_NAME).so` |
| `VENDOR_DIR` | `$(BUILD_DIR)/vendor-root` | Vendor overlay root |
| `DEPS_FLAGS` | empty | Flags for `check-deps` (e.g. `--strict`) |
| `XORG_DROID_SOCK` | `/tmp/x11droid.sock` | Socket for `make run` / `demo-producer` |

## Host Daemon (`host-daemon/Makefile`)

| Variable | Default | Effect |
|----------|---------|--------|
| `CC` | `gcc` | Compiler |
| `CFLAGS_COMMON` | inherited | Base C flags |
| `LDFLAGS` | empty | Extra linker flags |
| `BUILD_DIR` | `$(ROOT)/build` | Output directory |

Link line: `-lX11 -lXext -l:libGL.so.1 -lpthread -ldl`

## Android HWC2 (`android/hwcomposer/Makefile`)

| Variable | Default | Effect |
|----------|---------|--------|
| `NDK` | `$(ANDROID_NDK_HOME)` | NDK root |
| `API` | `30` | Android API level |
| `TARGET_ARCH` | `x86_64` | `x86_64`, `arm64`, `arm`, `x86` |
| `HOST_TAG` | `linux-x86_64` | NDK prebuilt host tag |
| `HWC_NAME` | `x11` | Output `hwcomposer.$(HWC_NAME).so` |
| `DESTDIR` | `$(VENDOR_DIR)` | Staging root for `install` |
| `VENDOR_LIBDIR` | `lib64` | Vendor library directory |
| `HOST_CXX` | `g++` | Compiler for `check` target |

## GLES Stream Client (`android/gles_stream/Makefile`)

| Variable | Default | Effect |
|----------|---------|--------|
| `NDK` | `$(ANDROID_NDK_HOME)` | NDK root |
| `API` | `30` | Android API level |
| `TARGET_ARCH` | `x86_64` | `x86_64`, `arm64`, `arm`, `x86` |
| `HOST_TAG` | `linux-x86_64` | NDK prebuilt host tag |
| `DESTDIR` | `$(VENDOR_DIR)` | Staging root for `install` |
| `VENDOR_LIBDIR` | `lib64` | Vendor library directory |

## Examples

```sh
# Cross-compile for arm64
make android ANDROID_ARCH=arm64 API=30

# Custom HWC name (e.g. redroid)
make android HWC_NAME=redroid
make setup HWC_NAME=redroid

# Custom vendor overlay location
make setup VENDOR_DIR=/path/to/vendor

# Strict dependency check
make check-deps DEPS_FLAGS="--strict"
make setup DEPS_FLAGS="--strict"
```