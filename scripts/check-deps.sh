#!/usr/bin/env bash
# language: Bash, file: scripts/check-deps.sh, target: Linux host (Ubuntu/Debian/Fedora/Arch)
# Check x11-droid host prerequisites: binder(binderfs), uinput, LXC, X11/XShm, GLX NVIDIA 470.
# Usage: scripts/check-deps.sh [--strict] [--no-color]
#   --strict    exit != 0 when any WARN is present (default: only FAIL)
#   --no-color  disable colors (automatically off when output is not a TTY)

set -uo pipefail

STRICT=0
COLOR=auto
for arg in "$@"; do
    case "$arg" in
        --strict)   STRICT=1 ;;
        --no-color) COLOR=0 ;;
        -h|--help)  sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done
if [[ "$COLOR" == auto && ! -t 1 ]]; then COLOR=0; fi

if [[ "$COLOR" == 1 ]]; then
    G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; B=$'\e[36m'; D=$'\e[2m'; BOLD=$'\e[1m'; N=$'\e[0m'
else
    G=''; Y=''; R=''; B=''; D=''; BOLD=''; N=''
fi

OK_N=0; WARN_N=0; FAIL_N=0; SKIP_N=0
PM=""
command -v apt-get >/dev/null 2>&1 && PM="sudo apt-get install -y"
command -v dnf     >/dev/null 2>&1 && PM="sudo dnf install -y"
command -v pacman  >/dev/null 2>&1 && PM="sudo pacman -S --needed"

section() { printf '\n%s== %s ==%s\n' "$BOLD$B" "$1" "$N"; }
ok()      { printf '  %s %-38s %s\n' "${G}[ OK ]${N}" "$1" "${2:-}"; OK_N=$((OK_N+1)); }
warn()    { printf '  %s %-38s %s\n' "${Y}[WARN]${N}" "$1" "${2:-}"; WARN_N=$((WARN_N+1)); }
fail()    { printf '  %s %-38s %s\n' "${R}[FAIL]${N}" "$1" "${2:-}"; FAIL_N=$((FAIL_N+1)); }
skip()    { printf '  %s %-38s %s\n' "${D}[SKIP]${N}" "$1" "${2:-}"; SKIP_N=$((SKIP_N+1)); }
hint()    { printf '         %s-> %s%s\n' "$D" "$1" "$N"; }
has_cmd() { command -v "$1" >/dev/null 2>&1; }

header() {
    printf '%s\n' "x11-droid — host dependency check"
    printf '%s\n' "kernel : $(uname -r)"
    printf '%s\n' "distro : $(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-unknown}" || echo unknown)"
    printf '%s\n' "date   : $(date '+%Y-%m-%d %H:%M:%S')"
}

# ---------------------------------------------------------------- toolchain
check_toolchain() {
    section "Build toolchain"
    local c
    for c in gcc g++ make pkg-config python3; do
        if has_cmd "$c"; then
            ok "$c" "$("$c" --version 2>/dev/null | head -n1 | cut -c1-48)"
        else
            fail "$c" "not found"
            [[ -n "$PM" ]] && hint "install: $PM build-essential pkg-config python3"
        fi
    done
    if has_cmd gcc && has_cmd g++; then
        local gcc_major
        gcc_major=$(gcc -dumpversion | cut -d. -f1)
        if (( gcc_major >= 9 )); then ok "gcc >= 9 (C11)" "major $gcc_major"
        else fail "gcc >= 9 (C11)" "major $gcc_major is too old"; fi
        if g++ -std=c++17 -x c++ -fsyntax-only /dev/null >/dev/null 2>&1; then
            ok "g++ -std=c++17" "supported"
        else fail "g++ -std=c++17" "flag is not supported"; fi
    fi
}

# ---------------------------------------------------------------- X11 / GLX
check_x11() {
    section "X11, XShm, and GLX"

    local pkg hdr name
    for pkg in x11 xext; do
        case "$pkg" in
            x11)  hdr="X11/Xlib.h";               label="libX11-dev" ;;
            xext) hdr="X11/extensions/XShm.h";    label="libXext-dev (XShm)" ;;
        esac
        if pkg-config --exists "$pkg" 2>/dev/null; then
            ok "$label" "pkg-config $(pkg-config --modversion "$pkg" 2>/dev/null)"
        elif [[ -f "/usr/include/$hdr" ]]; then
            ok "$label" "header present, .pc missing"
        else
            fail "$label" "header/pkg-config missing"
            [[ -n "$PM" ]] && hint "install: $PM libx11-dev libxext-dev"
        fi
    done

    if pkg-config --exists gl 2>/dev/null && [[ -f /usr/include/GL/glx.h ]]; then
        ok "libGL-dev (GLX)" "pkg-config $(pkg-config --modversion gl 2>/dev/null)"
    elif [[ -f /usr/include/GL/glx.h ]]; then
        ok "libGL-dev (GLX)" "header present, .pc missing"
    else
        fail "libGL-dev (GLX)" "/usr/include/GL/glx.h is missing"
        [[ -n "$PM" ]] && hint "install: $PM libgl1-mesa-dev libglx-dev"
    fi

    if [[ -n "${DISPLAY:-}" ]] && has_cmd xdpyinfo; then
        if xdpyinfo 2>/dev/null | grep -qx '[[:space:]]*XSHM'; then
            ok "XSHM extension (server ${DISPLAY})" "enabled"
        else
            fail "XSHM extension (server ${DISPLAY})" "not reported by server"
        fi
    elif [[ -z "${DISPLAY:-}" ]]; then
        skip "XSHM extension" "DISPLAY is not set (run from an Xorg session)"
    else
        skip "XSHM extension" "xdpyinfo is missing ($PM x11-utils)"
    fi

    if has_cmd glxinfo && [[ -n "${DISPLAY:-}" ]]; then
        local vendor ver
        vendor=$(glxinfo -B 2>/dev/null | sed -n 's/^server glx vendor string: //p' | head -n1)
        ver=$(glxinfo -B 2>/dev/null | sed -n 's/^OpenGL version string: //p' | head -n1)
        if [[ -n "$vendor" ]]; then
            ok "GLX server vendor" "${vendor:0:52}"
            [[ -n "$ver" ]] && ok "GLX OpenGL version" "${ver:0:52}"
        else
            warn "GLX server vendor" "glxinfo failed to query the server"
        fi
    else
        skip "GLX query" "requires DISPLAY + glxinfo ($PM mesa-utils)"
    fi
}

# ---------------------------------------------------------------- NVIDIA 470
check_nvidia() {
    section "NVIDIA 470 driver (legacy Kepler)"

    local ver=""
    if [[ -r /proc/driver/nvidia/version ]]; then
        ver=$(sed -n 's/^Kernel Module  \([0-9.]*\).*/\1/p' /proc/driver/nvidia/version | head -n1)
    fi
    if [[ -z "$ver" ]] && has_cmd nvidia-smi; then
        ver=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -n1 | tr -d ' ')
    fi

    if [[ -z "$ver" ]]; then
        fail "NVIDIA kernel module" "not loaded / GPU not visible"
        hint "ensure a Kepler card and driver 470 are installed; check: lsmod | grep nvidia"
    else
        local major=${ver%%.*}
        if [[ "$major" == "470" ]]; then
            ok "NVIDIA 470.x driver" "version $ver (project target)"
        else
            warn "NVIDIA driver is not 470.x" "version $ver — project targets legacy 470"
        fi
        if [[ -c /dev/nvidia0 && -w /dev/nvidia0 ]]; then
            ok "/dev/nvidia0" "present and writable"
        elif [[ -c /dev/nvidia0 ]]; then
            fail "/dev/nvidia0" "present but not writable (video group?)"
            hint "add your user to the video group: sudo usermod -aG video \$USER (relogin)"
        else
            fail "/dev/nvidia0" "device node is missing"
        fi
        if [[ -c /dev/nvidiactl ]]; then ok "/dev/nvidiactl" "present"
        else fail "/dev/nvidiactl" "device node is missing"; fi
    fi

    if [[ -f /proc/driver/nvidia/capabilities/gpu0/graphics ]]; then
        ok "NVIDIA graphics capability" "enabled"
    elif [[ -n "$ver" ]]; then
        warn "NVIDIA graphics capability" "capability file not found"
    fi
}

# ---------------------------------------------------------------- binder
kernel_config() {
    # $1 = option name regex; print the last value (y/m/n) from the kernel config
    local cfg=""
    if [[ -r /proc/config.gz ]]; then cfg=$(zcat /proc/config.gz 2>/dev/null)
    elif [[ -r "/boot/config-$(uname -r)" ]]; then cfg=$(<"/boot/config-$(uname -r)")
    fi
    [[ -n "$cfg" ]] && sed -n "s/^\(${1}\)=\(.*\)/\2/p" <<<"$cfg" | tail -n1
}

check_binder() {
    section "Binder (Android IPC)"

    local ipc binderfs
    ipc=$(kernel_config 'CONFIG_ANDROID_BINDER_IPC')
    binderfs=$(kernel_config 'CONFIG_ANDROID_BINDERFS')
    if [[ "$ipc" == "y" || "$ipc" == "m" ]]; then ok "CONFIG_ANDROID_BINDER_IPC" "= $ipc"
    else warn "CONFIG_ANDROID_BINDER_IPC" "not found in active config"; fi
    if [[ "$binderfs" == "y" || "$binderfs" == "m" ]]; then ok "CONFIG_ANDROID_BINDERFS" "= $binderfs"
    else skip "CONFIG_ANDROID_BINDERFS" "not found / not enabled"; fi

    if [[ -d /sys/module/binder_linux ]]; then
        ok "binder_linux module" "loaded"
    elif has_cmd modinfo && modinfo binder_linux >/dev/null 2>&1; then
        warn "binder_linux module" "available, not loaded"
        hint "load: sudo modprobe binder_linux devices=binder,hwbinder,vndbinder"
    else
        fail "binder_linux module" "not loaded and not available"
        hint "Ubuntu/Debian: sudo apt-get install linux-modules-extra-\$(uname -r)"
        hint "then: sudo modprobe binder_linux devices=binder,hwbinder,vndbinder"
    fi

    if grep -qw binder /proc/filesystems 2>/dev/null; then
        ok "binderfs (filesystem)" "registered in /proc/filesystems"
    else
        skip "binderfs (filesystem)" "not registered"
    fi

    if mountpoint -q /dev/binderfs 2>/dev/null; then
        ok "/dev/binderfs" "mounted"
        local dev found=0
        for dev in binder hwbinder vndbinder; do
            [[ -e "/dev/binderfs/$dev" ]] && found=$((found+1))
        done
        if (( found == 3 )); then ok "binder/hwbinder/vndbinder nodes" "complete in binderfs"
        elif (( found > 0 )); then warn "binder nodes in binderfs" "$found/3 available"
        else warn "binder nodes in binderfs" "none yet"; fi
    else
        skip "/dev/binderfs" "not mounted"
        hint "mkdir -p /dev/binderfs && sudo mount -t binder binder /dev/binderfs"
    fi

    local d have=0
    for d in /dev/binder /dev/hwbinder /dev/vndbinder; do
        [[ -c "$d" ]] && have=$((have+1))
    done
    if (( have == 3 )); then
        ok "/dev/binder,/dev/hwbinder,/dev/vndbinder" "complete"
    elif (( have > 0 )); then
        warn "binder device nodes" "$have/3 present"
    else
        fail "binder device nodes" "none present"
    fi
    [[ -c /dev/binder ]] && [[ ! -w /dev/binder ]] && \
        hint "apps require write access; configure group/udev for /dev/binder*"
    return 0
}

# ---------------------------------------------------------------- uinput
check_uinput() {
    section "Virtual input (uinput)"
    local node=0
    if [[ -c /dev/uinput ]]; then
        node=1
        if [[ -w /dev/uinput ]]; then ok "/dev/uinput" "present and writable"
        else
            fail "/dev/uinput" "present but not writable"
            hint "add your user to the input group: sudo usermod -aG input \$USER (relogin)"
        fi
    else
        fail "/dev/uinput" "device node is missing"
        hint "load the module: sudo modprobe uinput"
    fi

    if [[ -d /sys/module/uinput ]]; then ok "uinput module" "loaded"
    elif has_cmd modinfo && modinfo uinput >/dev/null 2>&1; then
        warn "uinput module" "available, not loaded"; hint "sudo modprobe uinput"
    elif (( node == 1 )); then
        skip "uinput module" "node exists; module is managed by the host/parent container"
    else
        fail "uinput module" "not loaded and not available"
        hint "kernel without INPUT_UINPUT — use a standard distro kernel"
    fi

    local rule=/etc/udev/rules.d/60-xorgdroid-uinput.rules
    if [[ -f "$rule" ]]; then ok "uinput udev rule" "$rule"
    else
        skip "uinput udev rule" "not present (optional, for persistent non-root access)"
        hint "echo 'KERNEL==\"uinput\", MODE=\"0660\", GROUP=\"input\"' | sudo tee $rule && sudo udevadm control --reload"
    fi
}

# ---------------------------------------------------------------- LXC
check_lxc() {
    section "LXC and container namespaces"

    local c missing=0
    for c in lxc-start lxc-info lxc-create lxc-attach; do
        if has_cmd "$c"; then ok "$c" "$(command -v "$c")"
        else fail "$c" "not found"; missing=$((missing+1)); fi
    done
    if (( missing > 0 )); then
        [[ -n "$PM" ]] && hint "install: $PM lxc lxc-templates uidmap"
    else
        local lv
        lv=$(lxc-info --version 2>/dev/null | head -n1)
        [[ -n "$lv" ]] && ok "LXC version" "$lv"
    fi

    if has_cmd lxc-checkconfig; then
        local bad
        bad=$(lxc-checkconfig 2>&1 | grep -c 'missing' || true)
        if [[ "$bad" == "0" ]]; then ok "lxc-checkconfig" "all features available"
        else warn "lxc-checkconfig" "$bad features marked missing"; hint "run: lxc-checkconfig"; fi
    else
        skip "lxc-checkconfig" "not present in this package"
    fi

    if [[ -f /sys/fs/cgroup/cgroup.controllers ]]; then
        ok "cgroup v2" "$(tr ' ' ',' < /sys/fs/cgroup/cgroup.controllers | sed 's/,$//')"
    elif [[ -d /sys/fs/cgroup ]]; then
        warn "cgroup v2" "not detected (legacy cgroup v1)"
    else
        fail "cgroup" "/sys/fs/cgroup is missing"; fi

    local ns missing_ns=0 nsname
    for nsname in user pid mnt net ipc uts cgroup; do
        [[ -e "/proc/self/ns/$nsname" ]] || missing_ns=$((missing_ns+1))
    done
    if (( missing_ns == 0 )); then ok "kernel namespaces" "user/pid/mnt/net/ipc/uts/cgroup"
    else fail "kernel namespaces" "$missing_ns namespaces missing"; fi

    local aal
    aal=$(kernel_config 'CONFIG_SECURITY_APPARMOR')
    if [[ "$aal" == "y" || "$aal" == "m" ]]; then
        if [[ -d /sys/kernel/security/apparmor ]]; then ok "AppArmor" "enabled"
        else skip "AppArmor" "in kernel, not active in userspace"; fi
    else
        skip "AppArmor" "not enabled (LXC can still run without a profile)"
    fi
}

# ---------------------------------------------------------------- summary
summary() {
    printf '\n%s== Summary ==%s\n' "$BOLD$B" "$N"
    printf '  %sOK%s: %d   %sWARN%s: %d   %sFAIL%s: %d   %sSKIP%s: %d\n' \
        "$G" "$N" "$OK_N" "$Y" "$N" "$WARN_N" "$R" "$N" "$FAIL_N" "$D" "$N" "$SKIP_N"

    if (( FAIL_N > 0 )); then
        printf '\n%sHost is NOT ready.%s Fix the FAIL items above and run: make check-deps\n' "$R$BOLD" "$N"
        return 1
    fi
    if (( STRICT == 1 && WARN_N > 0 )); then
        printf '\n%sStrict mode: %d WARN items treated as failures.%s\n' "$Y$BOLD" "$WARN_N" "$N"
        return 1
    fi
    printf '\n%sHost is ready to build and test host/daemon.%s\n' "$G$BOLD" "$N"
    return 0
}

header
check_toolchain
check_x11
check_nvidia
check_binder
check_uinput
check_lxc
summary
exit $?
