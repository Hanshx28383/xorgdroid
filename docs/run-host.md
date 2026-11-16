# Running on Host

## Quick Commands (Recommended)

```sh
# Quick test: Xvfb + daemon + 30 frames, auto-exit
make test
# or
./x11droid test

# Demo: daemon + test pattern (Ctrl+C to stop, or after N frames)
make demo
./x11droid demo [FRAMES]

# Quick status check
make status
./x11droid status
```

## Legacy Commands (Still Work)

```sh
# Build host components
make host

# Run daemon (foreground, Ctrl-C to stop)
make run-daemon
# or
./build/host-daemon/x11droid-daemon

# Send test pattern (in another terminal)
make demo-producer
# or finite frames:
./build/host-daemon/test_producer --frames 30
```

## Daemon Options

```sh
./build/host-daemon/x11droid-daemon \
  --display "$DISPLAY" \
  --socket /tmp/x11droid.sock \
  --shm x11droid_fb
```

| Flag | Effect |
|------|--------|
| `--display NAME` | X11 display (default: `$DISPLAY`) |
| `--socket PATH` | Unix socket path (default: `/tmp/x11droid.sock`) |
| `--shm NAME` | POSIX shm name (default: `x11droid_fb`) |
| `--no-gl` | Force CPU path (MIT-SHM/XPutImage), skip GLX |
| `--no-shm` | Force XPutImage, skip MIT-SHM |
| `--no-input` | Skip uinput device creation |
| `-h, --help` | Show help |

## Run with Xvfb (CPU Fallback Testing)

```sh
Xvfb :99 -screen 0 720x1280x24 &
export DISPLAY=:99
./build/host-daemon/x11droid-daemon --no-gl &
./build/host-daemon/test_producer --frames 30
```

For XPutImage fallback:
```sh
./build/host-daemon/x11droid-daemon --no-gl --no-shm &
```

## x11droid Script (Single-Command Wrapper)

```sh
# Quick test: auto Xvfb + daemon + 30 frames, auto-exit
./x11droid test [FRAMES]

# Demo: daemon + pattern, Ctrl+C to stop
./x11droid demo [FRAMES]

# Status check
./x11droid status

# Options
./x11droid test --headless --no-gl --no-shm --no-input
```

| Option | Effect |
|--------|--------|
| `--headless` | Force Xvfb (auto if DISPLAY empty) |
| `--no-gl` | Force CPU path (no GLX) |
| `--no-shm` | Force XPutImage (skip MIT-SHM) |
| `--no-input` | Skip uinput devices |
| `--images DIR` | Images dir for `up` (default: `container/images/`) |

## Daemon Behavior

- Creates 720×1280 X11 window
- Listens on `/tmp/x11droid.sock` for `xd_msg` refresh triggers
- Framebuffer at `/dev/shm/x11droid_fb` (POSIX shm, 3.6 MB)
- Creates uinput devices: `x11droid-touch` (abs 0..720×0..1280) + `x11droid-kbd`
- Press `Esc` in window to exit, or Ctrl-C in terminal

## Environment Variables

| Variable | Default | Purpose |
|----------|---------|---------|
| `DISPLAY` | required | X11 display |
| `XORG_DROID_SOCK` | `/tmp/x11droid.sock` | Socket for `make run` / `demo-producer` |
| `X11DROID_FB` | `x11droid_fb` | Shm name (override via `--shm`) |
| `X11DROID_SOCK` | `/tmp/x11droid.sock` | Socket override for `x11droid` script |
| `X11DROID_SHM` | `x11droid_fb` | Shm override for `x11droid` script |