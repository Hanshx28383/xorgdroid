# xorgdroid

`x11-droid` is a lightweight Android-container-to-native-X11 bridge for Linux
hosts, targeting the legacy NVIDIA 470 driver. It uses X11 and GLX directly:
no Wayland, no GBM, and no DMA-BUF on the host. Android pixels cross the
boundary through POSIX shared memory and Unix domain sockets.

The tree contains a host X11 daemon, an Android HWC2 HAL, and a GLES 2.0
command-streaming client. The host path works without a GPU; the GPU path
requires Xorg with GLX and the NVIDIA 470 driver.

## Architecture

```text
Android app / SurfaceFlinger
        |
        |  HWC2                         GLES 2.0
        v                                 v
hwcomposer.x11.so                 libGLES_x11droid.so
        |                                 |
        | /dev/shm/x11droid_fb            | /tmp/x11droid_gl.sock
        | /tmp/x11droid.sock              |
        v                                 v
                 x11droid-daemon
          X11 window + GLX context
                 |
                 v
            Xorg 720x1280 window
```

- HWC2 forces every layer to `HWC2_COMPOSITION_CLIENT`. SurfaceFlinger composes
  into the client target; `hwcomposer.x11.so` maps that buffer, converts it to
  BGRA8888 in shared memory, and signals the daemon.
- The daemon presents through MIT-SHM, `XPutImage`, or GLX. The MIT-SHM and
  `XPutImage` paths are CPU paths. GLX uploads the shared-memory buffer to a
  texture and draws it with the GPU.
- `libGLES_x11droid.so` forwards a GLES 2.0 subset to the daemon. The daemon
  executes commands inside the same GLX context used for window composition,
  serialized by a mutex.
- Optional uinput devices expose `x11droid-touch` and `x11droid-kbd` to the
  container.

## Repository layout

| Path | Purpose |
| --- | --- |
| `host-daemon/` | X11 window, MIT-SHM/XPutImage/GLX presentation, uinput input, GLES streaming server, test producer |
| `android/hwcomposer/` | HWC2 HAL (`hwcomposer.x11.so`) built with the Android NDK |
| `android/gles_stream/` | GLES 2.0 client (`libGLES_x11droid.so`) built with the Android NDK |
| `scripts/check-deps.sh` | Host dependency and capability check |
| `scripts/launch_container.sh` | LXC launcher: overlays, device binds, framebuffer and socket binds |
| `lxc-config/config` | LXC configuration template |
| `report.txt` | Implementation and test report (Indonesian) |
| `CONTRIBUTING.md` | Commit format, test requirements, and PR checklist |

## Requirements

### Host build and runtime

- Linux x86_64 host. The tested environment is a Linux container with Xvfb;
  a real Xorg session is required for GLX.
- C11 compiler, GNU Make, Xlib and XShm development headers, pthread, and
  `libGL.so.1` at link time.
- Xorg and the NVIDIA 470 driver for the GPU path. Xvfb has no GLX and only
  exercises the CPU paths.
- Root and LXC for the container launcher, plus kernel support for binderfs,
  uinput, loop devices, and overlayfs.
- A prebuilt Android 11 x86_64 `system.img` and `vendor.img`.
- Optional: Xvfb, `xwd`, `xdpyinfo`, `glxinfo`, and `simg2img` for tests and
  sparse-image handling.

Debian/Ubuntu packages:

```sh
sudo apt install build-essential libx11-dev libxext-dev libgl1-mesa-dev \
  libgl1 xvfb x11-apps x11-utils mesa-utils lxc
```

The NVIDIA 470 driver is a separate runtime installation. The build links
`libGL.so.1` directly, so a development symlink named `libGL.so` is not
required by the Makefiles. `libgl1-mesa-dev` is listed because
`scripts/check-deps.sh` checks for GLX headers.

### Android build

- Android NDK r23 or newer. The tested toolchain is NDK r30.
- API level 30 by default. The vendored HWC2/gralloc headers come from
  AOSP `android-11.0.0_r48`.
- No full AOSP build is required. The HAL and GLES client are standalone
  shared libraries.

## Build tutorial

### 1. Check the host

```sh
make check-deps
```

Use `make check-deps DEPS_FLAGS="--strict"` to treat warnings as failures.
`FAIL` means a hard prerequisite is missing; `WARN` is informational.

### 2. Build the host components

```sh
make host
```

Outputs:

```text
build/host-daemon/x11droid-daemon
build/host-daemon/test_producer
```

### 3. Build the Android components

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk
make android
```

Outputs:

```text
build/android/hwcomposer/hwcomposer.x11.so
build/android/gles_stream/libGLES_x11droid.so
```

Examples with build flags:

```sh
make android TARGET_ARCH=arm64 API=30
make android TARGET_ARCH=x86_64 API=30 HWC_NAME=redroid
make -C android/hwcomposer check          # host-only syntax check
```

### 4. Create a release archive

```sh
make dist
```

`make dist` requires the host and both Android outputs. It creates
`build/x11droid-release.tar.gz` with the binaries, scripts, LXC config,
README, `CONTRIBUTING.md`, and `report.txt`.

## Build flags

### Top level

| Variable | Default | Effect |
| --- | --- | --- |
| `CC` | `gcc` | Host C compiler |
| `CXX` | `g++` | Host C++ compiler |
| `BUILD_DIR` | `$(CURDIR)/build` | All build artifacts |
| `CFLAGS_COMMON` | C11, `-O2 -g`, warnings, `-Iinclude` | Exported to host components |
| `CXXFLAGS_COMMON` | C++17, `-O2 -g`, warnings, `-Iinclude` | Exported to host components |
| `LDFLAGS_COMMON` | `-Wl,--as-needed` | Exported to host components |
| `ANDROID_NDK_HOME` | unset | Enables the `android` target |
| `XORG_DROID_SOCK` | `/tmp/x11droid.sock` | Socket used by `make run` and `make demo-producer` |
| `DEPS_FLAGS` | empty | Extra flags for `scripts/check-deps.sh` |

### Host daemon

| Variable | Default | Effect |
| --- | --- | --- |
| `CC` | `gcc` | Compiler |
| `CFLAGS_COMMON` | same as top level | Base C flags |
| `LDFLAGS` | empty | Extra linker flags |
| `BUILD_DIR` | `$(ROOT)/build` | Output directory |

The link line uses `-lX11 -lXext -l:libGL.so.1 -lpthread -ldl`.

### Android HWC2

| Variable | Default | Effect |
| --- | --- | --- |
| `NDK` | `$(ANDROID_NDK_HOME)` | NDK root |
| `API` | `30` | Android API level |
| `TARGET_ARCH` | `x86_64` | `x86_64`, `arm64`, `arm`, or `x86` |
| `HOST_TAG` | `linux-x86_64` | NDK prebuilt host tag |
| `HWC_NAME` | `x11` | Output `hwcomposer.$(HWC_NAME).so` |
| `DESTDIR` | `build/vendor-root` | Staging root for `install` |
| `VENDOR_LIBDIR` | `lib64` | Vendor library directory |
| `HOST_CXX` | `g++` | Compiler for the `check` target |

### GLES stream client

| Variable | Default | Effect |
| --- | --- | --- |
| `NDK` | `$(ANDROID_NDK_HOME)` | NDK root |
| `API` | `30` | Android API level |
| `TARGET_ARCH` | `x86_64` | `x86_64`, `arm64`, `arm`, or `x86` |
| `HOST_TAG` | `linux-x86_64` | NDK prebuilt host tag |
| `DESTDIR` | `build/vendor-root` | Staging root for `install` |
| `VENDOR_LIBDIR` | `lib64` | Vendor library directory |

## Run on the host

Start the daemon in an Xorg session:

```sh
make run
```

In another terminal, send the test pattern:

```sh
make demo-producer
```

`make demo-producer` runs until interrupted; it does not stop after 30 frames.
For a finite run:

```sh
./build/host-daemon/test_producer --frames 30
```

Direct daemon options:

```sh
./build/host-daemon/x11droid-daemon \
  --display "$DISPLAY" \
  --socket /tmp/x11droid.sock \
  --shm x11droid_fb
```

Useful fallbacks:

```sh
./build/host-daemon/x11droid-daemon --no-gl      # force MIT-SHM/XPutImage
./build/host-daemon/x11droid-daemon --no-shm     # force XPutImage
./build/host-daemon/x11droid-daemon --no-input   # skip uinput devices
```

The daemon creates a 720x1280 window. Press Esc in the window to exit, or stop
it from the terminal that owns it. The producer exits with `--frames N`, or
runs forever by default.

## Deploy to an Android container

Stage the vendor libraries:

```sh
make -C android/hwcomposer install
make -C android/gles_stream install
```

The default staging tree is `build/vendor-root`:

```text
build/vendor-root/lib64/hw/hwcomposer.x11.so
build/vendor-root/lib64/libGLES_x11droid.so
```

Copy those files into the vendor overlay used by the container, or pass
`DESTDIR` to install directly into an unpacked vendor tree:

```sh
make -C android/hwcomposer install DESTDIR=/path/to/vendor
make -C android/gles_stream install DESTDIR=/path/to/vendor
```

Set the HWC property in `/vendor/build.prop`:

```properties
ro.hardware.hwcomposer=x11
```

For `HWC_NAME=redroid`, use `ro.hardware.hwcomposer=redroid` and install
`hwcomposer.redroid.so`.

To exercise the GLES client from a test process:

```sh
LD_PRELOAD=/vendor/lib64/libGLES_x11droid.so /path/to/test_app
```

Files must be readable by the Android process. In an enforcing image, apply
the vendor file label:

```sh
chmod 0644 /vendor/lib64/hw/hwcomposer.x11.so
chown 0:0 /vendor/lib64/hw/hwcomposer.x11.so
chcon u:object_r:vendor_file:s0 /vendor/lib64/hw/hwcomposer.x11.so
```

### Start the container

The host daemon must be running first: the launcher requires
`/dev/shm/x11droid_fb` and `/tmp/x11droid.sock`.

```sh
sudo scripts/launch_container.sh --images /path/to/images
```

The launcher reads `container/images/system.img` and
`container/images/vendor.img` by default, sets up overlayfs and device binds,
and starts LXC. Common options:

```sh
sudo scripts/launch_container.sh --help
sudo scripts/launch_container.sh --no-start
sudo scripts/launch_container.sh --stop
sudo scripts/launch_container.sh --clean --yes
```

Use `--system`, `--vendor`, `--fb`, and `--sock` when the images or host paths
are not at the defaults. The launcher needs root for loop mounts, overlayfs,
and binder device binds.

### Standard installation and Android boot

**1. Prepare Android images**
Place `system.img` and `vendor.img` (Android 11 x86_64) in `container/images/`:
```sh
mkdir -p container/images
# copy or symlink your images here:
# container/images/system.img
# container/images/vendor.img
```

**2. Build and stage vendor libraries**
```sh
export ANDROID_NDK_HOME=/path/to/android-ndk
make android
make -C android/hwcomposer install
make -C android/gles_stream install
```
This stages `hwcomposer.x11.so` and `libGLES_x11droid.so` under `build/vendor-root/`.

**3. Configure vendor overlay**
Copy the staged libraries into the vendor overlay used by the container
(e.g. `/var/lib/x11droid/overlay/vendor/upper/lib64/hw/` and
`/var/lib/x11droid/overlay/vendor/upper/lib64/`), then set the HWC property
in the overlay's `vendor/build.prop`:
```properties
ro.hardware.hwcomposer=x11
```
If `HWC_NAME` differs, use `ro.hardware.hwcomposer=<HWC_NAME>`.

**4. Ensure SELinux labels (enforcing images)**
```sh
chmod 0644 <overlay>/vendor/lib64/hw/hwcomposer.x11.so
chown 0:0 <overlay>/vendor/lib64/hw/hwcomposer.x11.so
chcon u:object_r:vendor_file:s0 <overlay>/vendor/lib64/hw/hwcomposer.x11.so
```

**5. Start host daemon (in an Xorg session)**
```sh
make run
```
The daemon creates a 720x1280 window and listens on `/tmp/x11droid.sock`
with the framebuffer at `/dev/shm/x11droid_fb`.

**6. Launch the Android container**
```sh
sudo scripts/launch_container.sh --images container/images
```
The container boots Android with binder, the framebuffer bind, and the
x11droid input devices. After boot, the Android UI appears in the daemon
window. Touch/keyboard events are forwarded through uinput.

**7. Verify GLES forwarding (optional)**
```sh
LD_PRELOAD=/vendor/lib64/libGLES_x11droid.so <android-test-app>
```

### Where to get Android x86_64 images

- **AOSP build**: `lunch aosp_x86_64-userdebug && make -j$(nproc) systemimage vendorimage`
  Output: `out/target/product/generic_x86_64/system.img` and `vendor.img`.
- **Prebuilt (Android 11, x86_64)**:
  - <https://ci.android.com/> → aosp_x86_64-userdebug → artifacts `system.img`, `vendor.img`
  - <https://github.com/redroid/redroid/releases> (redroid images include vendor)
  - <https://source.android.com/setup/develop/building/images> (official build instructions)

Sparse images (`.img` with `sparse` header) are accepted; the launcher runs
`simg2img` automatically if available.

## Testing

The CPU presentation paths can be tested under Xvfb:

```sh
make host
Xvfb :99 -screen 0 720x1280x24 &
export DISPLAY=:99
./build/host-daemon/x11droid-daemon --no-gl &
./build/host-daemon/test_producer --frames 30
```

Capture and compare a frame with `xwd`; the test report documents the exact
pixel check for frame 30. Repeat with `--no-shm` to exercise `XPutImage`
instead of MIT-SHM.

Other checks:

```sh
make -C android/hwcomposer check   # host syntax check, no NDK needed
readelf -d build/host-daemon/x11droid-daemon
readelf -d build/android/hwcomposer/hwcomposer.x11.so
```

`report.txt` records the build, functional tests, mock GLES protocol test,
HWC2 harness result, and the limits of the tested environment.

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `cannot open display` | `DISPLAY` is unset or Xorg is not reachable; run inside the X session and check `xhost`/`XAUTHORITY` |
| `libGL.so.1: cannot open shared object file` | Install the NVIDIA driver or a runtime `libGL`; the build needs the versioned library |
| `glXChooseVisual failed` | The X server has no usable GLX; use `--no-gl` for the CPU path |
| `server rejected XShmAttach` | The daemon and X server run as different users or in different namespaces; use `--no-shm` |
| `x11droid-touch` not found | uinput is unavailable or not writable; load `uinput`, add a udev rule, or run with `--no-input` |
| HWC module not loaded | Check the filename, `ro.hardware.hwcomposer`, permissions, and SELinux label |
| `framebuffer ... is missing` | Start `x11droid-daemon` before the container, or pass the matching `--fb` |
| `socket ... is missing` | Start the daemon or pass the matching `--sock` |
| NDK compiler not found | Set `ANDROID_NDK_HOME` to an extracted NDK r23+ tree and check `HOST_TAG` |
| LXC start fails | Run as root, verify binderfs/uinput/loop/overlayfs, and inspect the generated LXC config |

## Limitations

- The full GLX/GLES path requires Xorg with the NVIDIA 470 driver. Xvfb has no
  GLX, so this environment only validates the CPU fallback.
- Shaders are limited to a translated GLSL ES 1.00 subset targeting desktop
  GLSL 1.20. ES 3.x, OES extensions, and mediump/lowp precision are not
  implemented.
- `glVertexAttribPointer` and `glDrawElements` assume a bound VBO; client-side
  arrays are not supported.
- The HAL targets HWC2 API version 2.0 and API level 30 headers.
- Full SurfaceFlinger integration and loading the NDK `.so` files in a real
  Android image have not been validated here.
- GLES drawing and framebuffer present are not synchronized automatically;
  call `glFinish` or `glFlush` when ordering matters.
- `glGetString` returns desktop GL values, so `GL_VERSION` and
  `GL_SHADING_LANGUAGE_VERSION` may not look like Android values.

---

# xorgdroid (Bahasa Indonesia)

`x11-droid` adalah jembatan ringan dari kontainer Android ke X11 native pada
host Linux, dengan target driver legacy NVIDIA 470. Proyek ini memakai X11 dan
GLX secara langsung: tanpa Wayland, tanpa GBM, dan tanpa DMA-BUF di host.
Piksel Android melewati batas proses melalui POSIX shared memory dan Unix
domain socket.

Repo ini berisi daemon X11 host, HAL HWC2 Android, dan client GLES 2.0 yang
meneruskan perintah render. Jalur host dapat berjalan tanpa GPU; jalur GPU
memerlukan Xorg dengan GLX dan driver NVIDIA 470.

## Arsitektur

```text
Aplikasi Android / SurfaceFlinger
        |
        |  HWC2                         GLES 2.0
        v                                 v
hwcomposer.x11.so                 libGLES_x11droid.so
        |                                 |
        | /dev/shm/x11droid_fb            | /tmp/x11droid_gl.sock
        | /tmp/x11droid.sock              |
        v                                 v
                 x11droid-daemon
          jendela X11 + konteks GLX
                 |
                 v
            jendela Xorg 720x1280
```

- HWC2 memaksa semua layer menjadi `HWC2_COMPOSITION_CLIENT`. SurfaceFlinger
  mengomposisi ke client target; `hwcomposer.x11.so` memetakan buffer tersebut,
  mengonversinya ke BGRA8888 di shared memory, lalu memberi sinyal ke daemon.
- Daemon menampilkan frame melalui MIT-SHM, `XPutImage`, atau GLX. MIT-SHM dan
  `XPutImage` adalah jalur CPU. GLX mengunggah buffer shared memory ke tekstur
  dan menggambarnya dengan GPU.
- `libGLES_x11droid.so` meneruskan subset GLES 2.0 ke daemon. Daemon
  mengeksekusi perintah pada konteks GLX yang sama dengan komposisi jendela,
  diserialisasi oleh mutex.
- Perangkat uinput opsional menyediakan `x11droid-touch` dan `x11droid-kbd`
  untuk kontainer.

## Struktur repo

| Path | Fungsi |
| --- | --- |
| `host-daemon/` | Jendela X11, presentasi MIT-SHM/XPutImage/GLX, input uinput, server streaming GLES, test producer |
| `android/hwcomposer/` | HAL HWC2 (`hwcomposer.x11.so`) yang dibangun dengan NDK |
| `android/gles_stream/` | Client GLES 2.0 (`libGLES_x11droid.so`) yang dibangun dengan NDK |
| `scripts/check-deps.sh` | Pemeriksa dependensi dan kemampuan host |
| `scripts/launch_container.sh` | Launcher LXC: overlay, bind device, framebuffer, dan socket |
| `lxc-config/config` | Template konfigurasi LXC |
| `report.txt` | Laporan implementasi dan hasil uji (bahasa Indonesia) |
| `CONTRIBUTING.md` | Format commit, syarat pengujian, dan checklist PR |

## Kebutuhan

### Build dan runtime host

- Host Linux x86_64. Lingkungan yang diuji adalah kontainer Linux dengan Xvfb;
  sesi Xorg nyata diperlukan untuk GLX.
- Compiler C11, GNU Make, header Xlib dan XShm, pthread, serta `libGL.so.1`
  saat link.
- Xorg dan driver NVIDIA 470 untuk jalur GPU. Xvfb tidak menyediakan GLX dan
  hanya menguji jalur CPU.
- Root dan LXC untuk launcher kontainer, ditambah dukungan kernel untuk
  binderfs, uinput, loop device, dan overlayfs.
- Image Android 11 x86_64 `system.img` dan `vendor.img` yang sudah jadi.
- Opsional: Xvfb, `xwd`, `xdpyinfo`, `glxinfo`, dan `simg2img` untuk pengujian
  dan penanganan sparse image.

Paket Debian/Ubuntu:

```sh
sudo apt install build-essential libx11-dev libxext-dev libgl1-mesa-dev \
  libgl1 xvfb x11-apps x11-utils mesa-utils lxc
```

Driver NVIDIA 470 dipasang terpisah sebagai runtime. Build menautkan
`libGL.so.1` secara langsung, jadi symlink pengembangan bernama `libGL.so`
tidak diperlukan oleh Makefile. `libgl1-mesa-dev` dicantumkan karena
`scripts/check-deps.sh` memeriksa header GLX.

### Build Android

- Android NDK r23 atau lebih baru. Toolchain yang diuji adalah NDK r30.
- API level default 30. Header HWC2/gralloc yang disertakan berasal dari
  AOSP `android-11.0.0_r48`.
- Tidak perlu build AOSP penuh. HAL dan client GLES adalah shared library
  mandiri.

## Tutorial build

### 1. Periksa host

```sh
make check-deps
```

Gunakan `make check-deps DEPS_FLAGS="--strict"` agar warning dianggap gagal.
`FAIL` berarti prasyarat keras belum ada; `WARN` bersifat informatif.

### 2. Build komponen host

```sh
make host
```

Hasil:

```text
build/host-daemon/x11droid-daemon
build/host-daemon/test_producer
```

### 3. Build komponen Android

```sh
export ANDROID_NDK_HOME=/path/ke/android-ndk
make android
```

Hasil:

```text
build/android/hwcomposer/hwcomposer.x11.so
build/android/gles_stream/libGLES_x11droid.so
```

Contoh dengan build flag:

```sh
make android TARGET_ARCH=arm64 API=30
make android TARGET_ARCH=x86_64 API=30 HWC_NAME=redroid
make -C android/hwcomposer check          # syntax check di host
```

### 4. Buat arsip rilis

```sh
make dist
```

`make dist` memerlukan output host dan kedua output Android. Target ini membuat
`build/x11droid-release.tar.gz` berisi binary, script, konfigurasi LXC, README,
`CONTRIBUTING.md`, dan `report.txt`.

## Flag build

### Level teratas

| Variabel | Default | Fungsi |
| --- | --- | --- |
| `CC` | `gcc` | Compiler C host |
| `CXX` | `g++` | Compiler C++ host |
| `BUILD_DIR` | `$(CURDIR)/build` | Direktori seluruh artefak build |
| `CFLAGS_COMMON` | C11, `-O2 -g`, warning, `-Iinclude` | Diekspor ke komponen host |
| `CXXFLAGS_COMMON` | C++17, `-O2 -g`, warning, `-Iinclude` | Diekspor ke komponen host |
| `LDFLAGS_COMMON` | `-Wl,--as-needed` | Diekspor ke komponen host |
| `ANDROID_NDK_HOME` | kosong | Mengaktifkan target `android` |
| `XORG_DROID_SOCK` | `/tmp/x11droid.sock` | Socket untuk `make run` dan `make demo-producer` |
| `DEPS_FLAGS` | kosong | Flag tambahan untuk `scripts/check-deps.sh` |

### Host daemon

| Variabel | Default | Fungsi |
| --- | --- | --- |
| `CC` | `gcc` | Compiler |
| `CFLAGS_COMMON` | sama dengan level teratas | Flag C dasar |
| `LDFLAGS` | kosong | Flag linker tambahan |
| `BUILD_DIR` | `$(ROOT)/build` | Direktori output |

Baris link memakai `-lX11 -lXext -l:libGL.so.1 -lpthread -ldl`.

### Android HWC2

| Variabel | Default | Fungsi |
| --- | --- | --- |
| `NDK` | `$(ANDROID_NDK_HOME)` | Root NDK |
| `API` | `30` | Level API Android |
| `TARGET_ARCH` | `x86_64` | `x86_64`, `arm64`, `arm`, atau `x86` |
| `HOST_TAG` | `linux-x86_64` | Tag host prebuilt NDK |
| `HWC_NAME` | `x11` | Nama output `hwcomposer.$(HWC_NAME).so` |
| `DESTDIR` | `build/vendor-root` | Root staging untuk `install` |
| `VENDOR_LIBDIR` | `lib64` | Direktori library vendor |
| `HOST_CXX` | `g++` | Compiler untuk target `check` |

### Client GLES stream

| Variabel | Default | Fungsi |
| --- | --- | --- |
| `NDK` | `$(ANDROID_NDK_HOME)` | Root NDK |
| `API` | `30` | Level API Android |
| `TARGET_ARCH` | `x86_64` | `x86_64`, `arm64`, `arm`, atau `x86` |
| `HOST_TAG` | `linux-x86_64` | Tag host prebuilt NDK |
| `DESTDIR` | `build/vendor-root` | Root staging untuk `install` |
| `VENDOR_LIBDIR` | `lib64` | Direktori library vendor |

## Menjalankan di host

Jalankan daemon di sesi Xorg:

```sh
make run
```

Di terminal lain, kirim pola uji:

```sh
make demo-producer
```

`make demo-producer` berjalan terus sampai dihentikan; target ini tidak berhenti
setelah 30 frame. Untuk jumlah frame terbatas:

```sh
./build/host-daemon/test_producer --frames 30
```

Opsi langsung daemon:

```sh
./build/host-daemon/x11droid-daemon \
  --display "$DISPLAY" \
  --socket /tmp/x11droid.sock \
  --shm x11droid_fb
```

Fallback yang berguna:

```sh
./build/host-daemon/x11droid-daemon --no-gl      # paksa MIT-SHM/XPutImage
./build/host-daemon/x11droid-daemon --no-shm     # paksa XPutImage
./build/host-daemon/x11droid-daemon --no-input   # lewati perangkat uinput
```

Daemon membuka jendela 720x1280. Tekan Esc di jendela untuk keluar, atau
hentikan dari terminal yang menjalankannya. Producer berhenti dengan
`--frames N`, atau berjalan terus secara default.

## Penempatan di kontainer Android

Stage library vendor:

```sh
make -C android/hwcomposer install
make -C android/gles_stream install
```

Staging default berada di `build/vendor-root`:

```text
build/vendor-root/lib64/hw/hwcomposer.x11.so
build/vendor-root/lib64/libGLES_x11droid.so
```

Salin file tersebut ke overlay vendor yang dipakai kontainer, atau berikan
`DESTDIR` untuk memasang langsung ke vendor tree yang sudah diekstrak:

```sh
make -C android/hwcomposer install DESTDIR=/path/ke/vendor
make -C android/gles_stream install DESTDIR=/path/ke/vendor
```

Atur properti HWC di `/vendor/build.prop`:

```properties
ro.hardware.hwcomposer=x11
```

Untuk `HWC_NAME=redroid`, gunakan `ro.hardware.hwcomposer=redroid` dan pasang
`hwcomposer.redroid.so`.

Untuk menguji client GLES dari proses uji:

```sh
LD_PRELOAD=/vendor/lib64/libGLES_x11droid.so /path/ke/test_app
```

File harus dapat dibaca oleh proses Android. Pada image dengan SELinux
enforcing, terapkan label file vendor:

```sh
chmod 0644 /vendor/lib64/hw/hwcomposer.x11.so
chown 0:0 /vendor/lib64/hw/hwcomposer.x11.so
chcon u:object_r:vendor_file:s0 /vendor/lib64/hw/hwcomposer.x11.so
```

### Menjalankan kontainer

Daemon host harus berjalan lebih dulu: launcher memerlukan
`/dev/shm/x11droid_fb` dan `/tmp/x11droid.sock`.

```sh
sudo scripts/launch_container.sh --images /path/ke/images
```

Launcher membaca `container/images/system.img` dan
`container/images/vendor.img` secara default, menyiapkan overlayfs dan bind
device, lalu menjalankan LXC. Opsi umum:

```sh
sudo scripts/launch_container.sh --help
sudo scripts/launch_container.sh --no-start
sudo scripts/launch_container.sh --stop
sudo scripts/launch_container.sh --clean --yes
```

Gunakan `--system`, `--vendor`, `--fb`, dan `--sock` bila image atau path host
tidak berada di default. Launcher memerlukan root untuk loop mount, overlayfs,
dan bind device binder.

### Instalasi standar dan boot Android

**1. Siapkan image Android**
Taruh `system.img` dan `vendor.img` (Android 11 x86_64) di `container/images/`:
```sh
mkdir -p container/images
# salin atau symlink image Anda ke sini:
# container/images/system.img
# container/images/vendor.img
```

**2. Build dan stage library vendor**
```sh
export ANDROID_NDK_HOME=/path/ke/android-ndk
make android
make -C android/hwcomposer install
make -C android/gles_stream install
```
Ini menempatkan `hwcomposer.x11.so` dan `libGLES_x11droid.so` di
`build/vendor-root/`.

**3. Konfigurasi overlay vendor**
Salin library yang sudah di-stage ke overlay vendor yang dipakai kontainer
(cth. `/var/lib/x11droid/overlay/vendor/upper/lib64/hw/` dan
`/var/lib/x11droid/overlay/vendor/upper/lib64/`), lalu atur properti HWC
di `vendor/build.prop` overlay:
```properties
ro.hardware.hwcomposer=x11
```
Jika `HWC_NAME` berbeda, gunakan `ro.hardware.hwcomposer=<HWC_NAME>`.

**4. Pastikan label SELinux (image enforcing)**
```sh
chmod 0644 <overlay>/vendor/lib64/hw/hwcomposer.x11.so
chown 0:0 <overlay>/vendor/lib64/hw/hwcomposer.x11.so
chcon u:object_r:vendor_file:s0 <overlay>/vendor/lib64/hw/hwcomposer.x11.so
```

**5. Jalankan daemon host (di sesi Xorg)**
```sh
make run
```
Daemon membuat jendela 720x1280 dan mendengarkan di `/tmp/x11droid.sock`
dengan framebuffer di `/dev/shm/x11droid_fb`.

**6. Jalankan kontainer Android**
```sh
sudo scripts/launch_container.sh --images container/images
```
Kontainer boot Android dengan binder, bind framebuffer, dan perangkat input
x11droid. Setelah boot, UI Android muncul di jendela daemon.
Sentuhan/keyboard diteruskan lewat uinput.

**7. Verifikasi forwarding GLES (opsional)**
```sh
LD_PRELOAD=/vendor/lib64/libGLES_x11droid.so <android-test-app>
```

### Di mana mendapatkan image Android x86_64

- **Build AOSP**: `lunch aosp_x86_64-userdebug && make -j$(nproc) systemimage vendorimage`
  Output: `out/target/product/generic_x86_64/system.img` dan `vendor.img`.
- **Prebuilt (Android 11, x86_64)**:
  - <https://ci.android.com/> → aosp_x86_64-userdebug → artifacts `system.img`, `vendor.img`
  - <https://github.com/redroid/redroid/releases> (image redroid sudah include vendor)
  - <https://source.android.com/setup/develop/building/images> (petunjuk build resmi)

Image sparse (`.img` dengan header `sparse`) didukung; launcher menjalankan
`simg2img` otomatis jika tersedia.

## Pengujian

Jalur presentasi CPU dapat diuji dengan Xvfb:

```sh
make host
Xvfb :99 -screen 0 720x1280x24 &
export DISPLAY=:99
./build/host-daemon/x11droid-daemon --no-gl &
./build/host-daemon/test_producer --frames 30
```

Capture dan bandingkan frame dengan `xwd`; laporan uji mendokumentasikan
pemeriksaan piksel frame ke-30. Ulangi dengan `--no-shm` untuk menguji
`XPutImage`, bukan MIT-SHM.

Pemeriksaan lain:

```sh
make -C android/hwcomposer check   # syntax check host, tanpa NDK
readelf -d build/host-daemon/x11droid-daemon
readelf -d build/android/hwcomposer/hwcomposer.x11.so
```

`report.txt` mencatat build, uji fungsional, uji protokol GLES mock, hasil
harness HWC2, dan batas lingkungan pengujian.

## Pemecahan masalah

| Gejala | Penyebab dan solusi |
| --- | --- |
| `cannot open display` | `DISPLAY` kosong atau Xorg tidak dapat dijangkau; jalankan di sesi X dan periksa `xhost`/`XAUTHORITY` |
| `libGL.so.1: cannot open shared object file` | Pasang driver NVIDIA atau runtime `libGL`; build memerlukan library berversi tersebut |
| `glXChooseVisual failed` | X server tidak menyediakan GLX yang dapat dipakai; gunakan `--no-gl` untuk jalur CPU |
| `server rejected XShmAttach` | Daemon dan X server berjalan sebagai user atau namespace berbeda; gunakan `--no-shm` |
| `x11droid-touch` tidak ditemukan | uinput tidak tersedia atau tidak dapat ditulis; muat `uinput`, tambahkan aturan udev, atau jalankan dengan `--no-input` |
| Modul HWC tidak dimuat | Periksa nama file, `ro.hardware.hwcomposer`, permission, dan label SELinux |
| `framebuffer ... is missing` | Jalankan `x11droid-daemon` sebelum kontainer, atau berikan `--fb` yang sesuai |
| `socket ... is missing` | Jalankan daemon atau berikan `--sock` yang sesuai |
| Compiler NDK tidak ditemukan | Set `ANDROID_NDK_HOME` ke NDK r23+ yang sudah diekstrak dan periksa `HOST_TAG` |
| LXC gagal start | Jalankan sebagai root, pastikan binderfs/uinput/loop/overlayfs tersedia, dan periksa konfigurasi LXC yang dihasilkan |

## Batasan

- Jalur GLX/GLES penuh memerlukan Xorg dengan driver NVIDIA 470. Xvfb tidak
  memiliki GLX, sehingga lingkungan ini hanya memvalidasi fallback CPU.
- Shader dibatasi pada subset GLSL ES 1.00 yang diterjemahkan ke GLSL 1.20
  desktop. ES 3.x, ekstensi OES, dan presisi mediump/lowp belum diterapkan.
- `glVertexAttribPointer` dan `glDrawElements` mengasumsikan VBO terikat;
  client-side array belum didukung.
- HAL menargetkan HWC2 API versi 2.0 dan header API level 30.
- Integrasi SurfaceFlinger penuh dan pemuatan file `.so` NDK di image Android
  asli belum divalidasi di sini.
- Gambar GLES dan present framebuffer tidak disinkronkan otomatis; panggil
  `glFinish` atau `glFlush` bila urutan eksekusi penting.
- `glGetString` mengembalikan nilai GL desktop, sehingga `GL_VERSION` dan
  `GL_SHADING_LANGUAGE_VERSION` dapat berbeda dari nilai Android.
