# Container Deploy & Android Boot

## Overview

The container uses LXC with overlayfs:
- `system.img` / `vendor.img` as read-only lowerdirs
- Overlay upperdirs on host (persistent across power loss)
- Bind mounts: binder/hwbinder/vndbinder, framebuffer, socket, input devices

## Prepare Images

```sh
mkdir -p container/images
# Place Android 11 x86_64 images here:
# container/images/system.img
# container/images/vendor.img
```

Sparse images (Android `3aff26ed` magic) are auto-converted via `simg2img` if available.

### Where to Get Images

| Source | Notes |
|--------|-------|
| AOSP build | `lunch aosp_x86_64-userdebug && make -j$(nproc) systemimage vendorimage` |
| ci.android.com | aosp_x86_64-userdebug artifacts |
| redroid (Docker) | `docker pull redroid/redroid:11.0.0` — extract `system.img`/`vendor.img` from image |
| Official docs | <https://source.android.com/setup/develop/building/images> |

## Build & Stage Vendor Libraries

```sh
export ANDROID_NDK_HOME=/path/to/ndk
make android              # builds hwcomposer + libGLES
make -C android/hwcomposer install
make -C android/gles_stream install
```

Stages to `build/vendor-root/`:
```
build/vendor-root/lib64/hw/hwcomposer.x11.so
build/vendor-root/lib64/libGLES_x11droid.so
build/vendor-root/build.prop   # ro.hardware.hwcomposer=x11
```

## Configure Vendor Overlay

Copy staged libraries into the container's vendor overlay:

```sh
# Overlay upperdir (created by launch script at /var/lib/x11droid/overlay/vendor/upper/)
cp build/vendor-root/lib64/hw/hwcomposer.x11.so \
   /var/lib/x11droid/overlay/vendor/upper/lib64/hw/
cp build/vendor-root/lib64/libGLES_x11droid.so \
   /var/lib/x11droid/overlay/vendor/upper/lib64/
```

Set HWC property in overlay's `vendor/build.prop`:
```properties
ro.hardware.hwcomposer=x11
```

For custom `HWC_NAME` (e.g. `redroid`):
```properties
ro.hardware.hwcomposer=redroid
```

## SELinux (Enforcing Images)

```sh
chmod 0644 /vendor/lib64/hw/hwcomposer.x11.so
chown 0:0 /vendor/lib64/hw/hwcomposer.x11.so
chcon u:object_r:vendor_file:s0 /vendor/lib64/hw/hwcomposer.x11.so
```

## Start Host Daemon (Prerequisite)

**New (recommended):**
```sh
make run-daemon
# or
./x11droid test   # starts daemon + Xvfb
```

**Legacy:**
```sh
make run-daemon
# or
./build/host-daemon/x11droid-daemon
```

Daemon must be running before container launch — provides:
- `/dev/shm/x11droid_fb` (framebuffer)
- `/tmp/x11droid.sock` (refresh trigger)
- `x11droid-touch` / `x11droid-kbd` uinput devices

## Launch Container (Recommended)

```sh
# Daemon + container in one command (Ctrl+C stops everything)
make up
# or
sudo ./x11droid up [--images DIR]
```

**Legacy (daemon must be running first):**
```sh
sudo scripts/launch_container.sh start --images container/images
```

Options:
```sh
sudo scripts/launch_container.sh start --images /path/to/images
sudo scripts/launch_container.sh start --no-start   # prepare only
sudo scripts/launch_container.sh stop
sudo scripts/launch_container.sh clean --yes
scripts/launch_container.sh status
```

Legacy flags still work: `--stop`, `--clean --yes`, `--no-start`, `--images DIR`, `--system FILE`, `--vendor FILE`, `--fb PATH`, `--sock PATH`, `--name NAME`, `--state DIR`, `--lxcpath DIR`, `--yes`.

## Verify GLES Forwarding (Optional)

```sh
LD_PRELOAD=/vendor/lib64/libGLES_x11droid.so <android-test-app>
```

## Stop & Cleanup

```sh
make down
# or
./x11droid down
# or
sudo scripts/launch_container.sh stop
sudo scripts/launch_container.sh clean --yes
```

## Complete One-Liners

```sh
# Quick test (no container, no GPU, no root)
make test

# Demo: daemon + pattern, Ctrl+C to stop
make demo

# Full stack: daemon + container, Ctrl+C stops all
make up

# Stop everything
make down
```