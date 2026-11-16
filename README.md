# x11droid

> ⚠️ **Experimental** — Full SurfaceFlinger integration and GPU-accelerated GLX path not validated on real Android images. CPU fallback paths tested under Xvfb.

`x11droid` is a lightweight Android-container-to-native-X11 bridge for Linux hosts targeting the legacy NVIDIA 470 driver. It uses X11/GLX directly (no Wayland/GBM/DMA-BUF) and passes Android pixels via POSIX shared memory + Unix domain sockets.

**Prerequisites**
- Linux x86_64 with Xorg (GLX) or Xvfb (CPU fallback), NVIDIA 470 driver for GPU path
- Root + LXC + binderfs + uinput + loop/overlayfs kernel support
- Android 11 x86_64 `system.img` + `vendor.img` (place in `container/images/`)
- NDK r23+ for Android components (optional; host-only works without)

**Quick start**
```sh
git clone https://github.com/Hanshx28383/xorgdroid && cd x11droid
make setup       # check deps, build all, install to vendor overlay
make test        # quick test: Xvfb + daemon + 30 frames, auto-exit
make demo        # daemon + test pattern (Ctrl+C to stop)
sudo make up     # daemon + Android container (Ctrl+C stops all)
```

**Legacy (still works)**
```sh
make setup       # check deps, build all, install to vendor overlay
make start       # daemon + container (needs images in container/images/)
make stop        # stop container and daemon
```

**Documentation**
- [Architecture & data flows](docs/architecture.md)
- [Requirements & packages](docs/requirements.md)
- [Build flags & variables](docs/build-flags.md)
- [Run on host / daemon options](docs/run-host.md)
- [Container deploy & Android boot](docs/deploy.md)
- [Testing & pixel verification](docs/testing.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Limitations & known gaps](docs/limitations.md)
- [Build flags reference](docs/build-flags.md)

**Indonesian**: [README.id.md](README.id.md)

**License**: Apache-2.0 (AOSP headers) / project code MIT — see [LICENSE](LICENSE)