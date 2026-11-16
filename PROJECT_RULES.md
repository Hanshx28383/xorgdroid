# PROJECT_RULES.md — X11 Android Container Bridge (NVIDIA 470 Target)
## 0. Agentic Execution & Anti-Overthinking Protocol (CRITICAL)
- **NO ANALYSIS PARALYSIS:** Dilarang menghabiskan token berpikir untuk merancang arsitektur masa depan di luar langkah yang diminta saat ini. Berpikir maksimal 3 poin singkat (maksimal 50 kata internal), lalu **LANGSUNG tulis kode atau eksekusi tool**.
- **ONE FILE AT A TIME:** Jangan pernah mencoba membuat seluruh proyek dalam satu respons. Kerjakan dan selesaikan HANYA 1 file atau 1 fungsi per giliran.
- **NO CODE REFACTORING IN THOUGHTS:** Tulis versi pertama yang langsung bisa di-compile (`MVP`). Perbaikan dilakukan hanya jika ada error dari compiler.
- **CONCISE OUTPUT:** Jangan menulis penjelasan panjang lebar sebelum atau sesudah blok kode. Cukup berikan:
  1. Kode siap pakai (lengkap, tanpa placeholder `// TODO`).
  2. Satu baris perintah terminal untuk meng-compile dan mengujinya.

## 1. Visi & Batasan Utama (Strict Scope)
- **Tujuan:** Membangun *Android-on-Linux container bridge* dari nol yang berjalan secara *native* di **Xorg (X11)** dengan dukungan akselerasi grafis untuk **driver legacy NVIDIA 470 (Kepler)**.
- **DILARANG:**
  - JANGAN menyarankan atau menggunakan kompositor Wayland (`weston`, `cage`, `wlroots`, `waydroid` daemon bawaan).
  - JANGAN menginstruksikan kompilasi AOSP penuh dari nol (terlalu berat untuk hardware/listrik). Gunakan *prebuilt system/vendor image* x86_64 yang sudah ada.
  - JANGAN bergantung pada **GBM** atau **DMA-BUF** karena driver NVIDIA 470 tidak mendukungnya secara standar untuk *container sharing*.

## 2. Arsitektur Teknis Wajib
Proyek ini hanya terdiri dari 4 komponen terpisah yang ringan di-compile (< 2 menit di CPU low-end):
1. **Container Manager (Python/Shell + LXC):** Mengelola *namespace* LXC, *bind-mount* `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder`, dan *mount* `system.img`/`vendor.img`.
2. **X11 Host Daemon (C/C++ dengan Xlib/XCB + GLX):** Membuat jendela Xorg native, menerima *frame* dari kontainer via Unix Domain Socket / POSIX Shared Memory (`/dev/shm`), dan merendernya ke layar.
3. **Custom Android HWC (`hwcomposer.x11.so` - C++ via Android NDK):** Modul HAL di sisi Android (`vendor`) yang mencegat *layer* dari `SurfaceFlinger` dan mengirimkannya ke X11 Host Daemon.
4. **Input & GPU Translation Bridge:**
   - Input: Menerjemahkan `XEvent` (mouse/keyboard) ke `/dev/uinput` Linux untuk dibaca Android.
   - GPU (Tahap Lanjut): Translasi OpenGL ES (Android) -> OpenGL Desktop / GLX (NVIDIA 470 host) menggunakan VirGL / ANGLE atau *command streaming*, tanpa GBM/DMA-BUF.

## 3. Aturan Penulisan Kode & Workflow
- Semua kode C/C++ harus menggunakan standar minimal dependensi (C11 / C++17, `gcc`/`clang`, `Makefile` sederhana tanpa CMake yang rumit).
- Setiap perubahan harus modular dan bisa diuji secara terpisah (*decoupled*) sebelum digabung dengan *image* Android.
- Prioritaskan stabilitas dan manajemen memori eksplisit agar tidak terjadi *memory leak* di mesin dengan RAM terbatas.
