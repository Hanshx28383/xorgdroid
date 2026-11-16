# x11droid

> ⚠️ **Eksperimental** — Integrasi SurfaceFlinger penuh dan jalur GLX dengan GPU belum tervalidasi pada image Android asli. Jalur fallback CPU diuji di Xvfb.

`x11droid` adalah jembatan ringan dari kontainer Android ke X11 native pada host Linux, bertarget driver legacy NVIDIA 470. Proyek ini memakai X11/GLX langsung (tanpa Wayland/GBM/DMA-BUF) dan melewatkan piksel Android lewat POSIX shared memory + Unix domain socket.

**Prasyarat**
- Linux x86_64 dengan Xorg (GLX) atau Xvfb (fallback CPU), driver NVIDIA 470 untuk jalur GPU
- Root + LXC + binderfs + uinput + kernel support loop/overlayfs
- Android 11 x86_64 `system.img` + `vendor.img` (taruh di `container/images/`)
- NDK r23+ untuk komponen Android (opsional; host-only jalan tanpa NDK)

**Mulai cepat**
```sh
git clone https://github.com/Hanshx28383/xorgdroid && cd x11droid
make setup       # cek dependensi, build semua, pasang ke vendor overlay
sudo make start  # jalankan daemon + kontainer (butuh image di container/images/)
```

**Dokumentasi**
- [Arsitektur & alur data](docs/architecture.md)
- [Prasyarat & paket](docs/requirements.md)
- [Build flags & variabel](docs/build-flags.md)
- [Menjalankan di host / opsi daemon](docs/run-host.md)
- [Deploy kontainer & boot Android](docs/deploy.md)
- [Pengujian & verifikasi piksel](docs/testing.md)
- [Pemecahan masalah](docs/troubleshooting.md)
- [Batasan & kesenjangan](docs/limitations.md)
- [Referensi build flags](docs/build-flags.md)

**English**: [README.md](README.md)

**Lisensi**: Apache-2.0 (header AOSP) / kode proyek MIT — lihat [LICENSE](LICENSE)