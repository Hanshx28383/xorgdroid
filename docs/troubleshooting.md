# Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `cannot open display` | `DISPLAY` unset or Xorg unreachable | Run inside X session; check `xhost`/`XAUTHORITY` |
| `libGL.so.1: cannot open shared object file` | NVIDIA driver missing or libGL not in ldconfig | Install NVIDIA 470 driver; verify `ldconfig -p \| grep libGL.so.1` |
| `glXChooseVisual failed` | X server has no GLX | Use `--no-gl` for CPU path; ensure NVIDIA 470 + GLX |
| `server rejected XShmAttach` | Daemon/X server different user/namespace | Use `--no-shm`; or run daemon as same user as X server |
| `x11droid-touch` not found | uinput unavailable/not writable | `sudo modprobe uinput`; `sudo usermod -aG input $USER`; or `--no-input` |
| HWC module not loaded | Wrong filename / property / perms / SELinux | Check filename `hwcomposer.x11.so`; `ro.hardware.hwcomposer=x11`; `chmod 0644`; `chcon u:object_r:vendor_file:s0` |
| `framebuffer ... is missing` | Daemon not running | `make run-daemon` first; or pass `--fb PATH`; **or use `make up` which auto-starts daemon** |
| `socket ... is missing` | Daemon not running | `make run-daemon` first; or pass `--sock PATH`; **or use `make up` which auto-starts daemon** |
| NDK compiler not found | `ANDROID_NDK_HOME` unset or wrong | `export ANDROID_NDK_HOME=/path/to/ndk`; verify `HOST_TAG` |
| LXC start fails | Missing kernel features / not root | Run as root; verify binderfs/uinput/loop/overlayfs; check `lxc-checkconfig` |
| `binder devices not available` | binderfs not mounted / module not loaded | `sudo modprobe binder_linux devices=binder,hwbinder,vndbinder`; `sudo mount -t binder binder /dev/binderfs` |
| `/dev/uinput` present but not writable | User not in `input` group | `sudo usermod -aG input $USER` (relogin) |
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

## New Commands Auto-Start Daemon

The new `./x11droid up`, `./x11droid test`, and `./x11droid demo` commands **auto-start the daemon** if not running (with Xvfb if no DISPLAY). No manual `make run-daemon` needed.

## Diagnostic Commands

```sh
# Full diagnostic (checks deps, NDK, runtime, artifacts)
make doctor

# Dependency check
make check-deps

# Status of all artifacts + runtime
make status
./x11droid status

# Build logs
make host 2>&1 | tee build.log
export ANDROID_NDK_HOME=... && make android 2>&1 | tee android_build.log
```

## Quick Test Failures

| Command | Failure | Fix |
|---------|---------|-----|
| `./x11droid test` | `Xvfb failed to start on :99` | Port :99 busy; script auto-picks :98-:95; ensure Xvfb installed |
| `./x11droid test` | `daemon died` | Check log: `cat /tmp/x11droid-daemon.log`; try `--no-gl` `--no-input` |
| `./x11droid demo` | `daemon died` | Same as above; add `--no-gl --no-shm --no-input` |
| `./x11droid up` | `images dir not found` | Place `system.img` + `vendor.img` in `container/images/` or `--images DIR` |
| `./x11droid up` | `daemon failed to start` | Check DISPLAY; try `--headless --no-gl --no-input` |
| `make up` | `sudo: command not found` | Run with `sudo make up` or configure sudoers |