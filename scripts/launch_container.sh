#!/usr/bin/env bash
# language: Bash, file: scripts/launch_container.sh, target: Linux host (root)
# Launch an Android x86_64 image under LXC with:
#   - system.img / vendor.img as read-only overlayfs lowerdirs
#   - /data and the overlay upperdirs on the host (safe across power loss)
#   - binder/hwbinder/vndbinder, framebuffer, socket, and x11droid input binds
#
# Usage:
#   sudo scripts/launch_container.sh start       # prepare + start (default)
#   sudo scripts/launch_container.sh start --no-start  # prepare + mount only
#   sudo scripts/launch_container.sh stop        # stop container + release mounts
#   sudo scripts/launch_container.sh clean       # stop + remove overlay state (needs --yes)
#   sudo scripts/launch_container.sh status      # show container status
#   sudo scripts/launch_container.sh --help      # show this help
set -Eeuo pipefail
umask 022

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
TEMPLATE="$PROJECT_ROOT/lxc-config/config"

# Defaults (overridable via env or flags)
NAME="x11droid"
IMAGES_DIR="$PROJECT_ROOT/container/images"
SYSTEM_IMG=""
VENDOR_IMG=""
STATE_DIR="/var/lib/x11droid"
LXC_PATH="${LXC_PATH:-/var/lib/lxc}"
FB_PATH="/dev/shm/x11droid_fb"
SOCK_PATH="/tmp/x11droid.sock"

ACTION="start"
SUBACTION=""
DO_START=1
ASSUME_YES=0
KEEP_MOUNTS=0

# Derived paths (set by derive_paths)
SYSTEM_RO=""
SYSTEM_MNT=""
VENDOR_RO=""
VENDOR_MNT=""
OVL_SYS=""
OVL_VENDOR=""
ROOTFS=""
DATA_DIR=""
STATE_CONFIG=""
LOG_FILE=""
INIT_CMD=""
BINDER_ENTRIES=""
DEV_ENTRIES=""
INPUT_ENTRIES=""
NETWORK_ENTRIES=""

log()  { printf '[x11droid] %s\n' "$*"; }
warn() { printf '[x11droid] WARN: %s\n' "$*" >&2; }
die()  { printf '[x11droid] ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: $0 [action] [options]

Actions:
  start [--no-start]   prepare overlays, write LXC config, start container (default)
  stop                 stop container and release all overlay mounts
  clean [--yes]        stop container, release mounts, remove overlay state
  status               show container and runtime status
  help                 show this help

Options:
  --name NAME          LXC container name (default: x11droid)
  --images DIR         directory containing system.img and vendor.img (default: $PROJECT_ROOT/container/images)
  --system FILE        system.img path (overrides --images)
  --vendor FILE        vendor.img path (overrides --images)
  --state DIR          state/overlay directory (default: /var/lib/x11droid)
  --lxcpath DIR        LXC directory (default: /var/lib/lxc)
  --fb PATH            host POSIX shm framebuffer (default: /dev/shm/x11droid_fb)
  --sock PATH          host daemon Unix socket (default: /tmp/x11droid.sock)
  --no-start           prepare + mount, but do not run lxc-start (for start action)
  --yes                do not prompt for --clean confirmation
  -h, --help           show this help

Prerequisites for start:
  - system.img + vendor.img in --images directory
  - host daemon running (provides framebuffer + socket + input devices)
  - binderfs/uinput kernel support
  - LXC installed
  - run as root (sudo)

Examples:
  sudo $0 start --images /path/to/images
  sudo $0 stop
  sudo $0 clean --yes
  $0 status
EOF
}

# Require root immediately (except for help/status which are read-only)
require_root() {
    [ "$(id -u)" -eq 0 ] || die "must run as root: sudo $0 $*"
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required command '$1' not found"
}

derive_paths() {
    local mnt="$STATE_DIR/mnt"
    SYSTEM_RO="$mnt/system-ro"
    SYSTEM_MNT="$mnt/system"
    VENDOR_RO="$mnt/vendor-ro"
    VENDOR_MNT="$mnt/vendor"
    OVL_SYS="$STATE_DIR/overlay/system"
    OVL_VENDOR="$STATE_DIR/overlay/vendor"
    ROOTFS="$STATE_DIR/rootfs"
    DATA_DIR="$STATE_DIR/data"
    STATE_CONFIG="$STATE_DIR/config"
    LOG_FILE="$STATE_DIR/lxc.log"
}

release_mounts() {
    local p
    for p in "$VENDOR_MNT" "$SYSTEM_MNT" "$VENDOR_RO" "$SYSTEM_RO"; do
        [ -n "$p" ] || continue
        mountpoint -q "$p" 2>/dev/null || continue
        umount "$p" 2>/dev/null || {
            sleep 0.3
            umount "$p" 2>/dev/null
        } || {
            warn "mount is busy, lazy umount: $p"
            umount -l "$p" 2>/dev/null || true
        }
    done
    for p in "$VENDOR_MNT" "$SYSTEM_MNT" "$VENDOR_RO" "$SYSTEM_RO"; do
        [ -n "$p" ] || continue
        mountpoint -q "$p" 2>/dev/null && warn "mount is still present: $p"
    done
}

on_exit() {
    local rc=$?
    if [ "$rc" -ne 0 ] && [ "$KEEP_MOUNTS" -ne 1 ]; then
        release_mounts || true
    fi
}
trap on_exit EXIT

container_state() {
    command -v lxc-info >/dev/null 2>&1 || return 0
    lxc-info -P "$LXC_PATH" -n "$NAME" -sH 2>/dev/null | head -n1 || true
}

stop_container() {
    local state
    state=$(container_state)
    if [ "$state" = "RUNNING" ]; then
        log "stopping container $NAME"
        command -v lxc-stop >/dev/null 2>&1 || die "lxc-stop not found"
        lxc-stop -P "$LXC_PATH" -n "$NAME" -t 10 >/dev/null 2>&1 || true
        local i
        for i in $(seq 1 50); do
            state=$(container_state)
            [ "$state" != "RUNNING" ] && break
            sleep 0.2
        done
        [ "$state" != "RUNNING" ] || die "container did not stop: $NAME"
    elif [ -n "$state" ] && [ "$state" != "STOPPED" ]; then
        warn "container state: $state"
    fi
}

ensure_dirs() {
    mkdir -p \
        "$STATE_DIR/images" \
        "$OVL_SYS/upper" "$OVL_SYS/work" \
        "$OVL_VENDOR/upper" "$OVL_VENDOR/work" \
        "$STATE_DIR/mnt" \
        "$ROOTFS" "$DATA_DIR"
}

ensure_flat_image() {
    local img=$1 magic raw
    magic=$(od -An -tx1 -N4 "$img" | tr -d ' \n')
    if [ "$magic" = "3aff26ed" ]; then
        command -v simg2img >/dev/null 2>&1 || \
            die "$img is an Android sparse image; install simg2img (android-sdk-libsparse-utils) or convert it manually"
        raw="$STATE_DIR/images/$(basename "$img").raw"
        if [ ! -f "$raw" ]; then
            log "converting sparse -> raw: $(basename "$img") (one-time, may take a while)"
            simg2img "$img" "$raw"
        fi
        printf '%s\n' "$raw"
    else
        printf '%s\n' "$img"
    fi
}

resolve_images() {
    [ -n "$SYSTEM_IMG" ] || SYSTEM_IMG="$IMAGES_DIR/system.img"
    [ -n "$VENDOR_IMG" ] || VENDOR_IMG="$IMAGES_DIR/vendor.img"
    [ -f "$SYSTEM_IMG" ] || die "system.img not found: $SYSTEM_IMG (use --images DIR or --system FILE)"
    [ -f "$VENDOR_IMG" ] || die "vendor.img not found: $VENDOR_IMG (use --images DIR or --vendor FILE)"
    SYSTEM_IMG=$(ensure_flat_image "$SYSTEM_IMG")
    VENDOR_IMG=$(ensure_flat_image "$VENDOR_IMG")
    log "system: $SYSTEM_IMG"
    log "vendor: $VENDOR_IMG"
}

binder_src() {
    local dev=$1
    if [ -e "/dev/binderfs/$dev" ]; then printf '/dev/binderfs/%s\n' "$dev"; return 0; fi
    if [ -e "/dev/$dev" ]; then printf '/dev/%s\n' "$dev"; return 0; fi
    return 1
}

try_enable_binder() {
    if ! mountpoint -q /dev/binderfs 2>/dev/null; then
        if grep -qw binder /proc/filesystems 2>/dev/null; then
            mkdir -p /dev/binderfs
            mount -t binder binder /dev/binderfs 2>/dev/null || true
        fi
    fi
    if [ ! -e /dev/binder ] && command -v modprobe >/dev/null 2>&1; then
        modprobe binder_linux devices=binder,hwbinder,vndbinder 2>/dev/null || true
    fi
}

collect_binder_entries() {
    BINDER_ENTRIES=""
    local dev src missing=""

    for dev in binder hwbinder vndbinder; do
        binder_src "$dev" >/dev/null 2>&1 || missing="$missing $dev"
    done

    if [ -n "$missing" ]; then
        try_enable_binder
        missing=""
        for dev in binder hwbinder vndbinder; do
            binder_src "$dev" >/dev/null 2>&1 || missing="$missing $dev"
        done
    fi

    if [ -n "$missing" ]; then
        die "binder devices not available:$missing — fix with:
  sudo modprobe binder_linux devices=binder,hwbinder,vndbinder
  sudo mkdir -p /dev/binderfs && sudo mount -t binder binder /dev/binderfs
  (Ubuntu: sudo apt-get install linux-modules-extra-\$(uname -r))"
    fi

    if [ -d /dev/binderfs ]; then
        BINDER_ENTRIES+="lxc.mount.entry = /dev/binderfs dev/binderfs none bind,optional,create=dir 0 0"$'\n'
    fi
    for dev in binder hwbinder vndbinder; do
        src=$(binder_src "$dev")
        BINDER_ENTRIES+="lxc.mount.entry = $src dev/$dev none bind,optional,create=file 0 0"$'\n'
        log "binder: $src -> /dev/$dev"
    done
}

collect_input_entries() {
    INPUT_ENTRIES=""
    local f name ev node mm major minor found_touch=0 found_kbd=0

    for f in /sys/class/input/event*/device/name; do
        [ -r "$f" ] || continue
        name=$(<"$f")
        case "$name" in
            x11droid-touch) found_touch=1 ;;
            x11droid-kbd)   found_kbd=1 ;;
            *) continue ;;
        esac

        ev=$(basename "$(dirname "$(dirname "$f")")")
        node="/dev/input/$ev"
        if [ ! -e "$node" ]; then
            mm=$(<"/sys/class/input/$ev/dev")
            major=${mm%%:*}
            minor=${mm##*:}
            mkdir -p /dev/input
            mknod -m 660 "$node" c "$major" "$minor"
            warn "node $node was missing; created from sysfs ($mm)"
        fi
        INPUT_ENTRIES+="lxc.mount.entry = $node dev/input/$ev none bind,optional,create=file 0 0"$'\n'
        log "input: $node ($name) -> /dev/input/$ev"
    done

    if [ "$found_touch" -eq 0 ] || [ "$found_kbd" -eq 0 ]; then
        die "x11droid-touch/x11droid-kbd not found — start host daemon first:
  make run-daemon
  (or run host-daemon/build/x11droid-daemon manually)"
    fi
}

collect_device_entries() {
    read -r -d '' DEV_ENTRIES <<'EOF' || true
lxc.mount.entry = /dev/null dev/null none bind,optional,create=file 0 0
lxc.mount.entry = /dev/zero dev/zero none bind,optional,create=file 0 0
lxc.mount.entry = /dev/full dev/full none bind,optional,create=file 0 0
lxc.mount.entry = /dev/random dev/random none bind,optional,create=file 0 0
lxc.mount.entry = /dev/urandom dev/urandom none bind,optional,create=file 0 0
lxc.mount.entry = /dev/tty dev/tty none bind,optional,create=file 0 0
lxc.mount.entry = devpts dev/pts devpts newinstance,ptmxmode=0666,mode=0620,create=dir 0 0
lxc.mount.entry = /dev/fuse dev/fuse none bind,optional,create=file 0 0
lxc.mount.entry = /dev/ashmem dev/ashmem none bind,optional,create=file 0 0
lxc.mount.entry = /dev/net/tun dev/net/tun none bind,optional,create=file 0 0
EOF
}

collect_network_entries() {
    if command -v ip >/dev/null 2>&1 && ip link show lxcbr0 >/dev/null 2>&1; then
        NETWORK_ENTRIES=$'lxc.net.0.type = veth\nlxc.net.0.link = lxcbr0\nlxc.net.0.flags = up\n'
        log "network: veth via lxcbr0"
    else
        NETWORK_ENTRIES=$'lxc.net.0.type = none\n'
        warn "lxcbr0 bridge missing — container shares host network (type=none)"
    fi
}

ensure_daemon_running() {
    local timeout=10
    if [ -S "$SOCK_PATH" ] && [ -f "$FB_PATH" ]; then
        log "daemon already running (socket + framebuffer present)"
        return 0
    fi
    log "daemon not detected — attempting auto-start..."
    # Try to start daemon in background
    local daemon_bin="${PROJECT_ROOT}/build/host-daemon/x11droid-daemon"
    if [ ! -x "$daemon_bin" ]; then
        die "daemon binary not found: $daemon_bin — run 'make host' first"
    fi
    # Start daemon in background
    "$daemon_bin" --socket "$SOCK_PATH" --no-input >/dev/null 2>&1 &
    local daemon_pid=$!
    # Wait for socket and framebuffer to appear
    local i=0
    while [ $i -lt $timeout ]; do
        if [ -S "$SOCK_PATH" ] && [ -f "$FB_PATH" ]; then
            log "daemon started (pid $daemon_pid)"
            return 0
        fi
        sleep 1
        i=$((i+1))
    done
    kill $daemon_pid 2>/dev/null || true
    die "daemon failed to start within ${timeout}s — check DISPLAY and Xorg"
}

check_host_prereqs() {
    [ -f "$TEMPLATE" ] || die "config template missing: $TEMPLATE"
    # daemon check is now done by ensure_daemon_running()
}

mount_overlays() {
    mkdir -p "$SYSTEM_RO" "$SYSTEM_MNT" "$VENDOR_RO" "$VENDOR_MNT"

    mountpoint -q "$SYSTEM_RO" || \
        mount -o loop,ro,noatime "$SYSTEM_IMG" "$SYSTEM_RO"
    mountpoint -q "$VENDOR_RO" || \
        mount -o loop,ro,noatime "$VENDOR_IMG" "$VENDOR_RO"

    if ! mountpoint -q "$SYSTEM_MNT"; then
        mount -t overlay overlay \
            -o "lowerdir=$SYSTEM_RO,upperdir=$OVL_SYS/upper,workdir=$OVL_SYS/work" \
            "$SYSTEM_MNT" || die "system overlay failed; corrupt state? run: $0 clean --yes"
    fi
    if ! mountpoint -q "$VENDOR_MNT"; then
        mount -t overlay overlay \
            -o "lowerdir=$VENDOR_RO,upperdir=$OVL_VENDOR/upper,workdir=$OVL_VENDOR/work" \
            "$VENDOR_MNT" || die "vendor overlay failed; corrupt state? run: $0 clean --yes"
    fi

    log "system overlay : $SYSTEM_RO (ro) + $OVL_SYS/upper -> $SYSTEM_MNT"
    log "vendor overlay : $VENDOR_RO (ro) + $OVL_VENDOR/upper -> $VENDOR_MNT"
}

prepare_rootfs() {
    mkdir -p \
        "$ROOTFS"/dev/input "$ROOTFS"/dev/shm "$ROOTFS"/dev/pts \
        "$ROOTFS"/dev/net "$ROOTFS"/dev/socket \
        "$ROOTFS"/proc "$ROOTFS"/sys "$ROOTFS"/system "$ROOTFS"/vendor \
        "$ROOTFS"/data "$ROOTFS"/tmp "$ROOTFS"/mnt "$ROOTFS"/storage

    if [ ! -e "$ROOTFS/dev/ptmx" ] && [ ! -L "$ROOTFS/dev/ptmx" ]; then
        ln -s pts/ptmx "$ROOTFS/dev/ptmx"
    fi

    INIT_CMD="/system/bin/init"
    if [ -x "$SYSTEM_MNT/bin/init" ]; then
        log "Android init: /system/bin/init"
    elif [ -x "$SYSTEM_MNT/init" ]; then
        INIT_CMD="/init"
        ln -sfn system/init "$ROOTFS/init"
        log "Android init: /init (symlink to system/init)"
    else
        warn "init not found in system.img — lxc.init.cmd remains $INIT_CMD"
    fi
}

render_config() {
    local line
    : > "$STATE_CONFIG"
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            *"@BINDER_ENTRIES@"*)  printf '%s\n' "$BINDER_ENTRIES"  >> "$STATE_CONFIG"; continue ;;
            *"@DEV_ENTRIES@"*)     printf '%s\n' "$DEV_ENTRIES"     >> "$STATE_CONFIG"; continue ;;
            *"@INPUT_ENTRIES@"*)   printf '%s\n' "$INPUT_ENTRIES"   >> "$STATE_CONFIG"; continue ;;
            *"@NETWORK_ENTRIES@"*) printf '%s\n' "$NETWORK_ENTRIES" >> "$STATE_CONFIG"; continue ;;
        esac

        line=${line//@NAME@/$NAME}
        line=${line//@ROOTFS@/$ROOTFS}
        line=${line//@INIT_CMD@/$INIT_CMD}
        line=${line//@LOG@/$LOG_FILE}
        line=${line//@SYSTEM_MNT@/$SYSTEM_MNT}
        line=${line//@VENDOR_MNT@/$VENDOR_MNT}
        line=${line//@DATA_DIR@/$DATA_DIR}
        line=${line//@FB_PATH@/$FB_PATH}
        line=${line//@SOCK_PATH@/$SOCK_PATH}
        printf '%s\n' "$line" >> "$STATE_CONFIG"
    done < "$TEMPLATE"
    chmod 0644 "$STATE_CONFIG"
}

install_lxc_config() {
    install -d -m 0700 "$LXC_PATH/$NAME"
    install -m 0644 "$STATE_CONFIG" "$LXC_PATH/$NAME/config"
    log "LXC config: $LXC_PATH/$NAME/config"
}

start_container() {
    local state i
    state=$(container_state)
    if [ "$state" = "RUNNING" ]; then
        die "container $NAME is already running — use 'stop' first"
    fi

    need_cmd lxc-start
    need_cmd lxc-info

    log "starting lxc-start -n $NAME"
    if ! lxc-start -P "$LXC_PATH" -n "$NAME" -d; then
        [ -f "$LOG_FILE" ] && tail -n 20 "$LOG_FILE" >&2 || true
        die "lxc-start failed"
    fi

    state=""
    for i in $(seq 1 30); do
        state=$(container_state)
        [ "$state" = "RUNNING" ] && break
        sleep 0.2
    done

    if [ "$state" != "RUNNING" ]; then
        [ -f "$LOG_FILE" ] && tail -n 20 "$LOG_FILE" >&2 || true
        lxc-stop -P "$LXC_PATH" -n "$NAME" -t 5 >/dev/null 2>&1 || true
        die "container stopped during startup (init crash? check log)"
    fi

    KEEP_MOUNTS=1
    log "container $NAME is running"
    log "  attach: lxc-attach -P $LXC_PATH -n $NAME"
    log "  stop:   $0 stop --name $NAME --lxcpath $LXC_PATH"
}

do_status() {
    local state
    state=$(container_state)
    printf 'container %-12s %s\n' "$NAME" "${state:-STOPPED}"
    if [ -f "$STATE_CONFIG" ]; then
        printf 'config       %-12s %s\n' "" "present"
    else
        printf 'config       %-12s %s\n' "" "missing"
    fi
    if [ -f "/dev/shm/x11droid_fb" ]; then
        printf 'framebuffer  %-12s %s\n' "" "present"
    else
        printf 'framebuffer  %-12s %s\n' "" "missing"
    fi
    if [ -S "/tmp/x11droid.sock" ]; then
        printf 'daemon sock  %-12s %s\n' "" "present"
    else
        printf 'daemon sock  %-12s %s\n' "" "missing"
    fi
    if [ -c /dev/uinput ]; then
        printf 'uinput       %-12s %s\n' "" "present"
    else
        printf 'uinput       %-12s %s\n' "" "missing"
    fi
    if [ -f /var/lib/x11droid/config ]; then
        printf 'LXC config   %-12s %s\n' "" "present"
    else
        printf 'LXC config   %-12s %s\n' "" "missing"
    fi
}

do_start() {
    require_root
    need_cmd mount
    need_cmd umount
    need_cmd mountpoint
    need_cmd od

    [ -f "$TEMPLATE" ] || die "template missing: $TEMPLATE"
    [ "$(container_state)" = "RUNNING" ] && die "container $NAME is already running — use 'stop' first"

    resolve_images
    ensure_dirs
    release_mounts

    # Auto-start daemon if needed
    ensure_daemon_running

    collect_binder_entries
    collect_input_entries
    collect_device_entries
    collect_network_entries

    mount_overlays
    prepare_rootfs
    render_config
    install_lxc_config

    if [ "$DO_START" -eq 0 ]; then
        KEEP_MOUNTS=1
        log "mounts ready; container not started (use 'start' without --no-start)"
        log "  start manually: lxc-start -P $LXC_PATH -n $NAME"
        return 0
    fi

    start_container
}

do_stop() {
    require_root
    stop_container
    release_mounts
    log "container $NAME stopped; overlays and loop devices released"
}

do_clean() {
    require_root
    case "$STATE_DIR" in
        ""|"/"|*..*) die "STATE_DIR not safe to delete: $STATE_DIR" ;;
    esac

    stop_container
    release_mounts

    if [ "$ASSUME_YES" -ne 1 ]; then
        printf 'Delete %s and config %s/%s? [y/N] ' "$STATE_DIR" "$LXC_PATH" "$NAME"
        local answer
        read -r answer || answer=""
        case "$answer" in
            y|Y|yes|YES) ;;
            *) die "cancelled" ;;
        esac
    fi

    rm -rf -- "$STATE_DIR" "$LXC_PATH/$NAME"
    log "container state and config removed"
}

# Parse arguments - support both new subcommand style and legacy flags
LEGACY_MODE=0
while [ $# -gt 0 ]; do
    case "$1" in
        start|stop|clean|status|help) ACTION="$1"; shift ;;
        --stop)      ACTION="stop"; shift ;;
        --clean)     ACTION="clean"; shift ;;
        --no-start)  DO_START=0; shift ;;
        --yes)       ASSUME_YES=1; shift ;;
        --name)      NAME=${2:?--name requires argument}; shift 2 ;;
        --images)    IMAGES_DIR=${2:?--images requires argument}; shift 2 ;;
        --system)    SYSTEM_IMG=${2:?--system requires argument}; shift 2 ;;
        --vendor)    VENDOR_IMG=${2:?--vendor requires argument}; shift 2 ;;
        --state)     STATE_DIR=${2:?--state requires argument}; shift 2 ;;
        --lxcpath)   LXC_PATH=${2:?--lxcpath requires argument}; shift 2 ;;
        --fb)        FB_PATH=${2:?--fb requires argument}; shift 2 ;;
        --sock)      SOCK_PATH=${2:?--sock requires argument}; shift 2 ;;
        --yes)       ASSUME_YES=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *)           usage >&2; die "unknown argument: $1" ;;
    esac
done

[[ "$NAME" =~ ^[A-Za-z0-9][A-Za-z0-9_-]*$ ]] || die "invalid container name: $NAME"
derive_paths

# Require root for all actions except status/help
case "$ACTION" in
    status|help) ;;
    *) require_root ;;
esac

case "$ACTION" in
    start)  do_start ;;
    stop)   do_stop ;;
    clean)  do_clean ;;
    status) do_status ;;
    help)   usage ;;
    *)      usage >&2; die "unknown action: $ACTION" ;;
esac