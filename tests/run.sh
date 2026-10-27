#!/bin/sh
# Build the library, the usbfs emulator and the test program, then run
# every test under Wine against the emulated devices. No USB device needed.
#
#   tests/run.sh
#   WINE=/opt/wine-staging/bin/wine tests/run.sh
#
# Needs gcc, mingw-w64 (x86_64-w64-mingw32-gcc and -dlltool) and Wine with
# its development files. winegcc, winebuild and the Windows headers are
# looked for next to $WINE; on Debian and Ubuntu, with their own Wine:
#
#   WINEGCC=/usr/lib/wine/winegcc WINEBUILD=/usr/lib/wine/winebuild \
#   WINE_INCLUDE=/usr/include/wine/wine/windows tests/run.sh
#
# The tests run in a new Wine prefix that is deleted afterwards; set
# TEST_WINEPREFIX to keep one between runs. Nothing is installed: the
# library is found through WINEDLLPATH, and its placeholder sits next to
# the test program.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
src=$here/../src
WINE=${WINE:-wine}
wine=$(command -v "$WINE") || { echo "no Wine: set WINE" >&2; exit 1; }
bindir=$(dirname "$wine")
WINEGCC=${WINEGCC:-$bindir/winegcc}
WINEBUILD=${WINEBUILD:-$bindir/winebuild}
WINE_INCLUDE=${WINE_INCLUDE:-$bindir/../include/wine/windows}
WINESERVER=${WINESERVER:-$bindir/wineserver}
MINGW=${MINGW:-x86_64-w64-mingw32}

out=$(mktemp -d "${TMPDIR:-/tmp}/libusb0-test.XXXXXX")
prefix=${TEST_WINEPREFIX:-$out/prefix}
cleanup() {
    WINEPREFIX=$prefix "$WINESERVER" -k 2>/dev/null || true
    rm -rf "$out"
}
trap cleanup EXIT

echo "Building in $out"
mkdir -p "$out/build" "$out/bin" "$out/lib/x86_64-unix"
cp "$src"/*.c "$src"/*.h "$src"/libusb0.spec "$src"/Makefile.x64 "$out/build/"
make -s -C "$out/build" -f Makefile.x64 all libusb0.def \
    WINE_INCLUDE="$WINE_INCLUDE" WINEGCC="$WINEGCC" WINEBUILD="$WINEBUILD"
cp "$out/build/libusb0.dll.so" "$out/lib/x86_64-unix/"
cp "$out/build/libusb0.dll.fake" "$out/bin/libusb0.dll"
"$MINGW-dlltool" -d "$out/build/libusb0.def" -D libusb0.dll -l "$out/build/libusb0.a"
"$MINGW-gcc" -O2 -Wall -Wextra -I"$src" -o "$out/bin/libusb0-test.exe" \
    "$here/libusb0-test.c" "$out/build/libusb0.a"
# _FORTIFY_SOURCE would turn the open() it defines into a clash
gcc -O2 -Wall -Wextra -fPIC -shared -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -o "$out/usbemu.so" "$here/usbemu.c" -ldl -lpthread
gcc -O2 -Wall -Wextra -DUSBEMU_MKTREE -o "$out/usbemu-mktree" "$here/usbemu.c"

echo "Preparing a Wine prefix"
WINEPREFIX=$prefix WINEDEBUG=-all "$WINE" wineboot -i >/dev/null 2>&1 || true
WINEPREFIX=$prefix "$WINESERVER" -w 2>/dev/null || true

failed=0

# run NAME 'VAR=value ...' TEST... : run tests against a fresh device tree
run() {
    name=$1 settings=$2
    shift 2
    tree=$out/usb-$name
    "$out/usbemu-mktree" "$tree"
    : >"$out/$name.log"
    echo
    echo "== $name${settings:+ ($settings)}"
    # shellcheck disable=SC2086  # the settings are split into words on purpose
    env $settings USBEMU_ROOT="$tree" USB_DEVFS_PATH="$tree" USBEMU_LOG="$out/$name.log" \
        WINEPREFIX="$prefix" WINEDEBUG=-all WINEDLLOVERRIDES=libusb0=b \
        WINEDLLPATH="$out/lib" LD_PRELOAD="$out/usbemu.so" \
        timeout 180 "$WINE" "$out/bin/libusb0-test.exe" "$@" </dev/null \
        >"$out/$name.out" 2>&1 || echo "  FAIL  exit status $?" >>"$out/$name.out"
    tr -d '\r' <"$out/$name.out" | grep -v '^$' || true
    if grep -q "FAIL" "$out/$name.out"; then failed=$((failed + 1)); fi
}

# expect NAME PATTERN, refuse NAME PATTERN: what the devices must, or must
# not, have seen during run NAME
expect() {
    if grep -q "$2" "$out/$1.log"; then
        echo "  PASS  the devices saw: $2"
    else
        echo "  FAIL  the devices never saw: $2"
        failed=$((failed + 1))
    fi
}
refuse() {
    if grep -q "$2" "$out/$1.log"; then
        echo "  FAIL  the devices saw: $(grep "$2" "$out/$1.log" | head -n 1)"
        failed=$((failed + 1))
    else
        echo "  PASS  the devices never saw: $2"
    fi
}

run abi '' abi
run list '' list
refuse list 'claim\|setconfig\|detach'
run list-all 'LIBUSB0_ALL_DEVICES=1' list-all
run claim 'USBEMU_DRIVER=usb_serial_simple' claim
expect claim 'detach usb_serial_simple from 001/005'
refuse claim 'setconfig\|detach usbhid'
run keep 'USBEMU_DRIVER=usb_serial_simple LIBUSB0_DETACH_KERNEL_DRIVER=0' keep
refuse keep 'detach'
run transfers '' sync async threads
run unplug 'USBEMU_UNPLUG_MS=1500 USBEMU_TRAFFIC_HZ=50' unplug
expect unplug 'unplug 001/005'
run cycles '' cycles
run ant 'USBEMU_DRIVER=usb_serial_simple' ant
refuse ant 'setconfig\|detach usbhid'

echo
if [ "$failed" -eq 0 ]; then
    echo "ALL TESTS PASSED"
else
    echo "$failed FAILED"
    exit 1
fi
