# Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `cannot open display` | `DISPLAY` unset or Xorg unreachable | Run inside X session; check `xhost`/`XAUTHORITY` |
| `libGL.so.1: cannot open shared object file` | NVIDIA driver missing or libGL not in ldconfig | Install NVIDIA 470 driver; verify `ldconfig -p \| grep libGL.so.1` |
| `glXChooseVisual failed` | X server has no GLX | Use `--no-gl` for CPU path; ensure NVIDIA 470 + GLX |
| `server rejected XShmAttach` | Daemon/X server different user/namespace | Use `--no-shm`; or run daemon as same user as X server |
| `x11droid-touch` not found | uinput unavailable/not writable | `sudo modprobe uinput`; `sudo usermod -aG input $USER`; or `--no-input` |
| HWC module not loaded | Wrong filename / property / perms / SELinux | Check filename `hwcomposer.x11.so`; `ro.hardware.hwcomposer=x11`; `chmod 0644`; `chcon u:object_r:vendor_file:s0` |
| `framebuffer ... is missing` | Daemon not running | `make run-daemon` first; or pass `--fb PATH` |
| `socket ... is missing` | Daemon not running | `make run-daemon` first; or pass `--sock PATH` |
| NDK compiler not found | `ANDROID_NDK_HOME` unset or wrong | `export ANDROID_NDK_HOME=/path/to/ndk`; verify `HOST_TAG` |
| LXC start fails | Missing kernel features / not root | Run as root; verify binderfs/uinput/loop/overlayfs; check `lxc-checkconfig` |
| `binder devices not available` | binderfs not mounted / module not loaded | `sudo modprobe binder_linux devices=binder,hwbinder,vndbinder`; `sudo mount -t binder binder /dev/binderfs` |
| `/dev/uinput` present but not writable | User not in `input` group | `sudo usermod -aG input $USER` (relogin) |
| `framebuffer ... is missing` at container start | Daemon not started | Run `make run-daemon` before `make start` |
| `system.img not found` | Images not in `container/images/` | Place `system.img` + `vendor.img` in `container/images/` or use `--images DIR` |
| `simg2img` not found | Sparse image but tool missing | `sudo apt-get install android-sdk-libsparse-utils` |
| `container did not stop` | Init crash / hang | Check LXC log: `tail -20 /var/lib/x11droid/lxc.log` |
| `libGL-dev (GLX headers) missing` | GLX dev headers not installed | Not required for build (links `libGL.so.1` directly). For dev: `sudo apt-get install libgl1-mesa-dev libglx-dev` (may conflict with NVIDIA driver) |
| `binder_linux module not loaded` | Kernel config missing or module not loaded | `sudo modprobe binder_linux devices=binder,hwbinder,vndbinder`; install `linux-modules-extra-$(uname -r)` |
| `uinput module not loaded` | Kernel missing `CONFIG_INPUT_UINPUT` | Use distro kernel with uinput support |
| `container stopped during startup` | Android init crash / missing init | Check system.img has `/system/bin/init` or `/init`; verify image architecture (x86_64) |
| `make setup` fails at `make android` | NDK not found | Set `ANDROID_NDK_HOME` or install NDK to `~/Android/Sdk/ndk/` or `/opt/android-ndk` |
| `make dist` fails | Android artifacts missing | Run `make setup` or `export ANDROID_NDK_HOME=... && make all` first |
| `XSHM extension not reported` | X server lacks MIT-SHM | Use `--no-shm` fallback; or fix X server config |
| `GLX query requires DISPLAY + glxinfo` | No X display or mesa-utils missing | Run in X session; `sudo apt-get install mesa-utils` |

## Diagnostic Commands

```sh
# Full diagnostic
make doctor

# Dependency check
make check-deps

# Status of all artifacts + runtime
make status

# Build logs
make host 2>&1 | tee build.log
export ANDROID_NDK_HOME=... && make android 2>&1 | tee android_build.log
```