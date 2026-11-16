# Limitations & Known Gaps

## GPU / GLX Path

- **Full GLX/GLES path requires Xorg with NVIDIA 470 driver.**
  - Xvfb has no GLX extension; this environment only validates CPU fallback (MIT-SHM / XPutImage).
  - Real GPU testing requires physical NVIDIA 470 + Xorg with GLX.

## Shader Support

- **GLSL ES 1.00 subset only** → translated to desktop GLSL 1.20.
- **Not implemented:** ES 3.x, OES extensions, `mediump`/`lowp` precision qualifiers.
- Translation strips `#version`, `#extension`, precision qualifiers.
- Desktop GLSL 1.20 accepts `attribute`/`varying`/`texture2D`/`gl_FragColor`.

## Vertex / Draw

- `glVertexAttribPointer` / `glDrawElements` **assume bound VBO**.
- Client-side vertex arrays (no VBO) **not supported**.

## HWC / HAL

- Targets **HWC2 API version 2.0** (`0x2000001`) and **API level 30** headers.
- All layers forced to `HWC2_COMPOSITION_CLIENT` — SurfaceFlinger does all composition.
- **Full SurfaceFlinger integration unvalidated** — HAL logic tested via host harness only.
- Loading NDK `.so` files in real Android image (Bionic libc) **not tested here** — host harness uses glibc build of same source.

## GLES / GL

- **No explicit frame synchronization** between GLES draw and framebuffer present.
  - App must call `glFinish()` / `glFlush()` when ordering matters.
- `glGetString` returns **desktop GL values** (e.g., `GL_VERSION` = desktop 4.x, not "OpenGL ES 2.0").
  - `GL_VERSION` / `GL_SHADING_LANGUAGE_VERSION` may not look like Android values.
- **No client-side arrays** — VBO required.
- `glVertexAttribPointer` / `glDrawElements` assume VBO bound.

## Input

- uinput touch/keyboard **not validated in current test runs** (daemon run with `--no-input`).
- `/dev/uinput` required; udev rule optional for persistent non-root access.

## Container / Android Image

- **Android 11 x86_64 only** (API 30 headers).
- Sparse images supported (auto `simg2img`).
- **No ARM/arm64 runtime validation** — cross-compile only.
- Redroid images are Docker images; need extraction to raw `system.img`/`vendor.img`.

## Build / Tooling

- Host daemon links `-l:libGL.so.1` (no `libGL.so` symlink needed).
- `libgl1-mesa-dev` **not required for build**; only for `check-deps` GLX header check (may conflict with NVIDIA driver).
- NDK auto-detection searches: `$ANDROID_NDK_HOME`, `~/Android/Sdk/ndk/*`, `/opt/android-ndk*`, `/usr/lib/android-ndk*`.
- No full AOSP build required — standalone HAL + GLES client.

## Test Environment Gaps

| Feature | Validated Here | Needs Real Environment |
|---------|----------------|------------------------|
| CPU framebuffer (MIT-SHM) | ✅ Xvfb, pixel-exact | — |
| CPU framebuffer (XPutImage) | ✅ Xvfb, pixel-exact | — |
| Shared memory layout | ✅ header seq, size | — |
| HWC2 host harness | ✅ glibc build, 44 funcs | ❌ Bionic load in Android |
| Mock GLES protocol | ✅ framing, ID mapping | ❌ Real GL execution |
| GLES shader translation | ✅ ES 1.00 → GLSL 1.20 | ❌ ES 3.x / extensions |
| uinput touch/keyboard | ❌ not tested | ❌ Real device |
| SurfaceFlinger integration | ❌ not tested | ❌ Real Android image |
| GPU GLX path | ❌ no NVIDIA 470 | ❌ Physical GPU |
| SELinux enforcement | ❌ not tested | ❌ Enforcing image |

## Known Workarounds

| Gap | Workaround |
|-----|------------|
| No GLX on Xvfb | Use `--no-gl` (CPU path) |
| No MIT-SHM (diff user/ns) | Use `--no-shm` (XPutImage) |
| No uinput | Use `--no-input` |
| Missing NDK | Set `ANDROID_NDK_HOME` |
| Missing images | Download from ci.android.com or build AOSP |
| Sparse image | Install `simg2img` (`android-sdk-libsparse-utils`) |
| SELinux denial | `chcon u:object_r:vendor_file:s0` on `.so` files |