This is libusb-win32 for Wine.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

Some Windows applications use libusb-win32. This Wine DLL allows to run
such Windows applications under Wine using native Linux USB stack.

It is a builtin `libusb0.dll` that talks to devices through usbfs
(`/dev/bus/usb`), so no Windows driver is involved. It behaves the way
libusb-win32 does on Windows, down to its timeouts, error codes and the
`GetLastError()` values it leaves behind, because programs depend on
those. Zwift's ANT+ support (through `ANT_DLL.dll`) is one program that
works with it.

## Building

You need gcc, make, and Wine's development files: winegcc, winebuild and
the Windows headers.

With the Wine that Debian or Ubuntu ship
(`sudo apt install libwine-dev wine64-tools`):

    $ cd src
    $ make -f Makefile.x64 WINEGCC=/usr/lib/wine/winegcc WINEBUILD=/usr/lib/wine/winebuild

With WineHQ's packages (`sudo apt install wine-staging-dev`, or the
`-stable` or `-devel` one), point the build at them:

    $ cd src
    $ make -f Makefile.x64 WINE_INCLUDE=/opt/wine-staging/include/wine/windows \
          WINEGCC=/opt/wine-staging/bin/winegcc WINEBUILD=/opt/wine-staging/bin/winebuild

That builds `libusb0.dll.so`, the library, and `libusb0.dll.fake`, its
placeholder. `Makefile` builds a 32-bit library for 32-bit programs; it
needs a 32-bit Wine that is not built in the new WoW64 mode, and has not
been tested with current Wine.

## Installing

Wine loads a builtin DLL only if it finds a file of that name where
Windows would look for it: in `system32` or next to the program. That
file is the placeholder; the library itself is looked for in Wine's own
folders, then in the folders listed in `WINEDLLPATH`.

**Into Wine**, for every prefix. For Debian's or Ubuntu's Wine:

    $ sudo make -f Makefile.x64 install-lib

and for WineHQ's:

    $ sudo make -f Makefile.x64 install-lib dlldir=/opt/wine-staging/lib/wine

Wine puts the placeholder into a prefix when it creates the prefix or
updates it, which it does after a Wine upgrade; `wineboot -u` updates an
existing prefix now. (`install-dev` adds the import library that winegcc
needs to build winelib programs such as `lsusb-wine`.)

**Into one prefix**, without root:

    $ mkdir -p ~/wine-dlls/x86_64-unix
    $ cp libusb0.dll.so ~/wine-dlls/x86_64-unix/
    $ cp libusb0.dll.fake "$WINEPREFIX/drive_c/windows/system32/libusb0.dll"
    $ WINEDLLPATH=~/wine-dlls wine program.exe

**If the program brings its own `libusb0.dll`**, as most do, Wine loads
that one, and it cannot work without its Windows driver. Make Wine use
the builtin instead, whatever file it finds:

    $ wine reg add 'HKCU\Software\Wine\DllOverrides' /v libusb0 /d builtin /f

or in `winecfg`, on the Libraries tab. To check which one is loaded:

    $ WINEDEBUG=+loaddll wine program.exe 2>&1 | grep -i libusb0

## Device access

usbfs serves only those who may write to the device's node, so a Windows
program only gets to see the devices your login can write to. A udev
rule gives you a device; for an ANT USB-m stick (vendor `0fcf`, product
`1009`), `/etc/udev/rules.d/70-libusb-wine.rules` would hold:

    SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0fcf", ATTR{idProduct}=="1009", MODE="0660", TAG+="uaccess"

`TAG+="uaccess"` hands the device to whoever is logged in at the screen,
and only works in a file numbered below 73. Then run
`sudo udevadm control --reload-rules` and plug the device in again.

## How it treats devices

- It lists the devices this user may open for writing, and no hubs:
  what libusb-win32 lists is the devices bound to its driver, the ones a
  program can use. Every listed device hangs off one virtual hub per
  bus, as with libusb-win32.
- It leaves kernel drivers alone. The exception is a short list of
  devices that Linux gives a driver and Windows does not, so far the ANT
  USB2 and USB-m sticks (`usb_serial_simple` binds them as serial
  ports): when a program claims one of their interfaces, it is taken from
  the kernel driver. Never from another program.
- `usb_set_configuration()` for the configuration a device is already
  in does nothing, as in libusb-win32.

## Settings

All are environment variables.

| Variable | Effect |
|---|---|
| `USB_DEBUG=1`...`4` | messages on stderr, more with each level |
| `LIBUSB0_ALL_DEVICES=1` | list every device, hubs and read-only ones too |
| `LIBUSB0_DETACH_KERNEL_DRIVER=1` | take any claimed interface from its kernel driver |
| `LIBUSB0_DETACH_KERNEL_DRIVER=0` | never take one, not even an ANT stick's |
| `USB_DEVFS_PATH` | where usbfs is, if not in `/dev/bus/usb` |

## What is missing

These libusb-win32 functions are not there:

 * usb_install_service_np, usb_uninstall_service_np, usb_install_driver_np,
   usb_touch_inf_file_np, usb_install_needs_restart_np, usb_install_npA,
   usb_install_npW: they install Windows drivers, which mean nothing here
 * usb_isochronous_setup_async: no isochronous transfers
 * usb_reset_ex

## Testing

`tests/run.sh` runs the tests under Wine against emulated devices, so it
needs no hardware. [HACKING.md](HACKING.md) explains how the library
works, and what to keep in mind when changing it.
