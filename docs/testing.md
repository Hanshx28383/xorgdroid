# Testing & Pixel Verification

## CPU Path Tests (Xvfb)

### MIT-SHM Path

```sh
make host
Xvfb :99 -screen 0 720x1280x24 &
export DISPLAY=:99
./build/host-daemon/x11droid-daemon --no-gl &
./build/host-daemon/test_producer --frames 30
```

Capture frame:
```sh
xwd -root -silent -out shot.xwd
```

Verify pixels (Python):
```python
import struct
d = open('shot.xwd', 'rb').read()
hdr = struct.unpack('>25I', d[:100])
pix_offset = hdr[0] + hdr[19] * 12  # header + XWDColor table
w, h, bpl = hdr[4], hdr[5], hdr[12]
def px(x, y):
    o = pix_offset + y * bpl + x * 4
    b, g, r, a = d[o:o+4]
    return f"{a:02x}{r:02x}{g:02x}{b:02x}"
print("center=" + px(w//2, h//2), "topleft=" + px(0, 0), "bottomright=" + px(w-1, h-1))
```

**Expected (frame 30):**
```
center=ffae5f10 topleft=ffe80d32 bottomright=ffffffff
MATCH=True
```

### XPutImage Fallback (`--no-shm`)

```sh
./build/host-daemon/x11droid-daemon --no-gl --no-shm &
./build/host-daemon/test_producer --frames 30
```

Same expected pixels, `MATCH=True`.

## Shared Memory Header Check

```sh
./build/host-daemon/x11droid-daemon &
./build/host-daemon/test_producer --frames 30
python3 -c "
import struct, os
d=open('/dev/shm/x11droid_fb','rb').read(64)
m,v,w,h,s,bpp,fmt,seq=struct.unpack('<8I',d[:32])
print(f'magic=0x{m:08x} ver={v} w={w} h={h} stride={s} bpp={bpp} fmt={fmt} seq={seq}')
print(f'size={os.path.getsize(\"/dev/shm/x11droid_fb\")} expected={64+720*1280*4}')
"
```

**Expected:**
```
magic=0x31424458 version=1 w=720 h=1280 stride=2880 bpp=4 fmt=0 seq=30
size=3686464 expected=3686464
```

## HWC2 Host Harness

```sh
g++ -std=c++17 -fPIC -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread \
  -Iandroid/hwcomposer/include -Iandroid/hwcomposer \
  -shared -Wl,--no-undefined -Wl,--exclude-libs,ALL \
  -o /tmp/hwcomposer_host.so android/hwcomposer/hwcomposer_x11.cpp \
  android/hwcomposer/xd_shm.cpp android/hwcomposer/xd_gralloc.cpp

g++ -std=c++17 -O2 -g -Wall -Wextra \
  -Iandroid/hwcomposer/include -Iandroid/hwcomposer \
  -o /tmp/hwc_harness hwc_harness.cpp -ldl

/tmp/hwc_harness
```

**Expected:**
```
HMI tag=0x48574d54 id=hwcomposer name=x11droid hwcomposer
open rc=0
version=0x2000001 (HWC_DEVICE_API_VERSION_2_0)
getFunction found=44 missing=77
getCapabilities count=0
registerCallback hotplug=0 refresh=0 vsync=0
getDisplayConfigs rc=0 count=1
setVsync enable=0 disable=0 emitted=5 (~16ms period)
close rc=0
HWC_RC=0
```

## Mock GLES Protocol Test

```sh
gcc -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra \
  -I/tmp/opencode/shim -Iandroid/gles_stream/include \
  -o /tmp/gles_harness /tmp/opencode/gles_client_harness.c \
  android/gles_stream/src/gles_client.c -ldl -lpthread

rm -f /tmp/x11droid_gl.sock
python3 /tmp/opencode/mock_gles_server.py >/tmp/mock.log 2>&1 & MPID=$!
sleep 0.5
/tmp/gles_harness
kill $MPID
```

**Expected client output:**
```
shader=1
status=1 log='hello' len=5
prog=2 loc=3
buf=3,4
px0=0 px15=15
version='mock-GL'
```

**Expected server sequence:**
```
PASS sequence: [1, 2, 4, 5, 7, 8, 9, 34, 39, 15, 59, 61, 12, 6]
```

## Static Checks

```sh
readelf -d build/host-daemon/x11droid-daemon
readelf -d build/android/hwcomposer/hwcomposer.x11.so
readelf -d build/android/gles_stream/libGLES_x11droid.so
nm -D --defined-only build/android/gles_stream/libGLES_x11droid.so | grep -c ' gl'
```

**Expected:**
- Daemon: PIE x86-64, NEEDED libX11, libXext, libGL.so.1, libc
- HWC: ELF Bionic x86-64, SONAME hwcomposer.x11.so, NEEDED liblog, libdl, libc
- GLES: ELF Bionic x86-64, SONAME libGLES_x11droid.so, 62 `gl*` symbols exported

## Dist Package

```sh
make dist
```

Produces `build/x11droid-release.tar.gz` with:
```
x11droid-daemon
hwcomposer.x11.so
libGLES_x11droid.so
launch_container.sh
check-deps.sh
config
README.md
CONTRIBUTING.md
report.txt
```