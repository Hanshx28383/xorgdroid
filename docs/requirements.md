# Requirements & Packages

## Host Build & Runtime

| Requirement | Purpose |
|-------------|---------|
| Linux x86_64 | Host architecture |
| Xorg + NVIDIA 470 | GPU path (GLX) |
| Xvfb | CPU fallback testing |
| C11 compiler (gcc ≥ 9) | Host daemon build |
| GNU Make | Build system |
| libX11-dev, libXext-dev | X11 + XShm headers |
| pthread, libdl | Threading, dynamic loading |
| libGL.so.1 (runtime) | NVIDIA driver provides; no dev symlink needed |
| Root + LXC | Container launcher |
| binderfs / binder_linux | Android IPC |
| uinput | Virtual touch/keyboard |
| loop, overlayfs | Image mounting, overlays |
| Android 11 x86_64 system.img + vendor.img | Container rootfs |

### Debian/Ubuntu Packages

```sh
# Build + runtime (host)
sudo apt install build-essential libx11-dev libxext-dev \
  libgl1 xvfb x11-apps x11-utils mesa-utils lxc

# Note: libgl1-mesa-dev NOT required for build (links libGL.so.1 directly).
# Install only if you need GLX headers for other projects.
# NVIDIA 470 driver provides libGL.so.1 at runtime.
```

### Kernel Config (verify with `make check-deps`)

```
CONFIG_ANDROID_BINDER_IPC=y|m
CONFIG_ANDROID_BINDERFS=y|m
CONFIG_INPUT_UINPUT=y|m
CONFIG_OVERLAY_FS=y
CONFIG_BLK_DEV_LOOP=y
CONFIG_USER_NS=y
CONFIG_PID_NS=y
CONFIG_NET_NS=y
CONFIG_IPC_NS=y
CONFIG_UTS_NS=y
CONFIG_CGROUP_V2=y
```

## Android Build

- NDK r23+ (tested r30)
- API 30 (Android 11)
- Vendored AOSP headers from `android-11.0.0_r48`
- No full AOSP build needed
- Target arches: x86_64 (default), arm64, arm, x86

## Optional Test Tools

- `xwd`, `x11-apps` — frame capture
- `xdpyinfo`, `x11-utils` — X11 extension checks
- `glxinfo`, `mesa-utils` — GLX vendor/version query
- `simg2img` (`android-sdk-libsparse-utils`) — sparse image conversion