# Working on libusb-wine

Notes for whoever changes this code next: how the pieces fit, what
libusb-win32 behaviour is reproduced and why, and which kernel behaviour
the code relies on.

## The pieces

| File | What it is |
|---|---|
| `src/usb.c`, `src/descriptors.c`, `src/error.c` | libusb 0.1's portable part: the device list, descriptor parsing, the error string |
| `src/linux.c` | everything that touches usbfs: listing devices, configurations and interfaces, transfers, the asynchronous API |
| `src/usb-wine.c` | the Wine side: `DllMain()`, `usb_get_version()`, `SetLastError()` |
| `src/usb.h` | the API, with libusb-win32's structure layout (`#pragma pack(1)`) |
| `src/libusb0.spec` | what the DLL exports |
| `tests/` | an emulated usbfs and a Windows test program; see Testing |

## How Wine runs it

`libusb0.dll.so` is a winelib DLL: an ELF shared object that Wine loads
into the Windows process and that Windows code calls directly, with no
thunk in between. Much follows from that.

**The calling convention.** Windows code calls with the Microsoft x64
convention; gcc on Linux compiles for System V. Every exported function
carries `LIBUSB_API` (`__attribute__((ms_abi))`), and gcc refuses a
definition whose attribute differs from its prototype in `usb.h`. A new
export needs a prototype there, with `LIBUSB_API`, and a line in
`libusb0.spec`; `-Werror=missing-prototypes` catches a forgotten prototype.

**Two worlds in one library.** `linux.c` is ordinary Linux code: it uses
pthreads, `poll()`, `eventfd` and Linux headers. Wine's Windows headers
stay out of it, because they clash with the system ones. The one Windows
call the library needs, `SetLastError()`, goes through
`usbwine_set_last_error()` in `usb-wine.c`, which is compiled with Wine's
headers.

**Loading.** A program finds the DLL by its name: Wine looks for a file
called `libusb0.dll` in the program's folder, in `system32`, and so on.
If that file is Wine's placeholder (`libusb0.dll.fake`), or if the
`libusb0` DLL override says `builtin`, Wine loads the builtin, which it
looks for in its own folders first and then in `WINEDLLPATH`. Without
any `libusb0.dll` file the load fails, builtin or not. `README.md` gives
the commands.

## libusb-win32 behaviour, and how it is reproduced

Windows programs are written and tested against libusb-win32, so its
behaviour is the specification, quirks included. Its source
(`libusb/src/windows.c` and `error.c` in libusb-win32 1.2.x) is the
reference wherever this file says "as libusb-win32 does".

| | libusb-win32 | here |
|---|---|---|
| devices listed | those bound to its driver | those this user may open for writing; no hubs |
| `bus->root_dev` | a virtual hub with every device as a child | the same |
| `usb_open()` of a device that cannot be opened | `NULL` | `NULL` |
| configuration after `usb_open()` | the device's current one, cached by the driver | the current one, from sysfs |
| `usb_set_configuration()` to the current one | nothing | nothing (Linux would reset the endpoints) |
| `usb_set_configuration()` with an interface claimed | `-EINVAL` | `-EINVAL` |
| `usb_claim_interface()` without a configuration | `-EINVAL` | `-EINVAL` |
| `usb_claim_interface()` of the claimed interface | `0` | `0` |
| `usb_close()` | releases the claimed interface | the same |
| errors returned | `-EINVAL`, `-ETRANSFER_TIMEDOUT` (116), `-ENOMEM`, `-EIO` | Linux errors folded into the same four (`win_error()`) |
| bulk vs. interrupt transfers | the same driver request | the same: `USBDEVFS_BULK`, or bulk URBs |
| synchronous timeout 0 | no timeout | no timeout |
| `usb_reap_async()` timeout | `WaitForSingleObject()`'s: 0 polls, negative waits for ever | the same |
| after a reap times out | `usb_reap_async()` cancels, `_nocancel()` does not | the same |
| reaping a finished request again | the same result | the same result |
| `usb_cancel_async()` | aborts every request on the endpoint, synchronously | the same |
| `usb_submit_async()` | needs a configuration and a claimed interface | the same |
| last error after a failed reap | `ERROR_OPERATION_ABORTED` (cancelled, unplugged), `ERROR_GEN_FAILURE` (failed) | the same |
| last error after a timed-out reap | whatever was there before | `ERROR_SEM_TIMEOUT` |

The last line is a deliberate difference. `ANT_DLL` reads a timeout with
`ERROR_OPERATION_ABORTED` as "the stick lost power" and starts over, and
a value left over from an earlier cancel would make an ordinary timeout
look like that.

Two more differences, both on the side of safety: `usb_free_async()`
stops a request that is still running (libusb-win32 frees the context
and lets the driver write into it later), and a reap that times out with
cancel returns the data if the request finished just before the cancel.

## Taking devices from kernel drivers

On Windows a device used through libusb-win32 is bound to the libusb0
driver and nothing else. On Linux the kernel may have bound a driver of
its own, and a program can only claim the interface if that driver lets
go. The library never makes it let go, except for the devices in
`take_from_kernel[]` in `linux.c`. For those, `usb_claim_interface()`
uses `USBDEVFS_DISCONNECT_CLAIM` with `EXCEPT_DRIVER "usbfs"`, which takes
the interface from a kernel driver in one step but never from another
program using usbfs.

The list is kept short on purpose. Programs probe: `ANT_DLL` opens every
listed device, sets configuration 1 and claims interface 0, just to see
whether it can. With the old behaviour, which detached interface 0's
driver inside `usb_set_configuration()`, every device the user could
write to lost its driver that way: a phone, a camera, a game controller.
A device belongs on the list when
Linux binds it to a driver that a Windows user of libusb-win32 would
never have, as `usb_serial_simple` does with the ANT sticks. Add it with
its vendor and product ID and the driver's name in a comment.
`LIBUSB0_DETACH_KERNEL_DRIVER=1` is the escape hatch for anything else.

## The asynchronous API

### Why it needs an engine

usbfs has one completion queue per open file, not one per request.
`REAPURB` hands out the next finished URB, whoever submitted it. A
program like `ANT_DLL` keeps a read pending on one thread, reaps it with
a one-second timeout, and writes from another thread, so two threads
reaping independently would take each other's completions.

### How it works

Everything lives in `struct linux_handle`, one per open device, under
its mutex:

- Each context (`struct usb_async`) is `IDLE` (never submitted, or the
  last submit failed), `PENDING`, or `DONE` with a result and a last
  error.
- **Only `drain_locked()` reaps.** It calls `REAPURBNDELAY` until the
  queue is empty and files every URB with the context it belongs to,
  found by the URB's address.
- **Waiting** (`wait_for()`): one waiting thread at a time sleeps in
  `poll()` on the device with the lock released. usbfs reports `POLLOUT`
  while a finished URB is queued. When it wakes it drains, which files
  everyone's results and broadcasts the condition variable. The other
  waiting threads sleep on that condition variable and take over `poll()`
  when the poller leaves; they also drain now and then on their own, in
  case the poller is held up.
- A thread that files a result while another thread is in `poll()`
  writes to an `eventfd` that is part of that `poll()`, so the poller
  never sleeps through a result that someone else collected for it.
- **Unplugging**: `REAPURBNDELAY` returns `ENODEV` once the device is gone
  and nothing is left to reap, and `poll()` returns at once with `POLLHUP`.
  `h->gone` records it, and waiting threads then nap 10 ms at a time
  until the kernel has given everything back, which takes milliseconds.
- **Closing** (`async_close()`): discards everything pending, drains, and
  sets `h->closing`; from then on nobody reaps. Requests still pending
  are marked aborted, threads in `wait_for()` are woken and leave, and
  `async_close()` waits up to two seconds for them before freeing the
  state. If a thread does not leave (it was suspended, say), the state
  and the device's file are left open for it rather than freed under it.

### Rules to keep

1. Nothing but `drain_locked()` calls a reap ioctl.
2. Nobody sleeps holding the lock: `poll()` runs with it released, and
   condition-variable waits release it.
3. The kernel writes to a context's URB (status, length, data) only when
   that URB is reaped. A context the kernel still has must therefore stay
   allocated until it is reaped: `usb_free_async()` marks such a context
   `orphaned`, and `complete_locked()` frees it when it comes back. Once
   `closing` is set nothing is reaped any more, so orphans can be freed.
4. Synchronous transfers use `USBDEVFS_BULK`, never the completion queue.
5. Calling `usb_close()` while another thread is still *entering* a call
   on the same handle is the program's bug, as it is on Windows. Threads
   already waiting in `usb_reap_async()` are handled.

## Kernel behaviour relied on

From `drivers/usb/core/devio.c` (checked against Linux 6.8):

- Completions are queued per file. `REAPURBNDELAY` returns `-EAGAIN`
  when the queue is empty, and `-ENODEV` once the device is gone and the
  queue is empty; reaping is allowed after a disconnect.
- `DISCARDURB` kills the URB synchronously (`usb_kill_urb()`), so it is on
  the completion queue when the ioctl returns. It fails with `EINVAL` if
  the URB is no longer pending.
- `poll()` reports `POLLOUT` while the completion queue is not empty (for
  a file opened for writing), `POLLHUP` once disconnected, and `POLLERR`
  once the file has been dropped from the device.
- Every request needs a file opened for writing; otherwise `EPERM`.
- `USBDEVFS_BULK` unlocks the device while it waits, so other requests on
  the same file proceed; it works on interrupt endpoints too; a timeout
  of 0 means none; it returns `ETIMEDOUT`; it copies data back only on
  success; its wait can only be interrupted by a fatal signal.
- A bulk URB on an interrupt endpoint is turned into an interrupt
  transfer; an interrupt URB on a bulk endpoint is refused with `EINVAL`.
- `SETCONFIGURATION` fails with `EBUSY` while any interface has a driver,
  and for the current configuration it resets the device's endpoints.
- `DISCONNECT_CLAIM` with `EXCEPT_DRIVER "usbfs"` detaches any driver but
  usbfs and claims the interface in one step.
- Data and status reach the program's memory when a URB is reaped, not
  when it completes.
- `open()` of a device node resumes a suspended device.

## Testing

`tests/run.sh` builds the library, `tests/usbemu.c` and
`tests/libusb0-test.c`, then runs each test in a fresh Wine prefix with
the emulator preloaded. `usbemu.so` stands in for usbfs on a fake
`/dev/bus/usb` holding four devices: an ANT USB-m stick that answers the
ANT requests `ANT_DLL` makes, a receiver bound to `usbhid` (which must
never be taken), a hub, and a device this user cannot write to. It
logs what the devices see, and `run.sh` checks that log too: for
instance, that listing touches no device and that `usbhid` is never
detached.

To test something new, add a function to `libusb0-test.c` and a `run`
line to `run.sh`; emulator behaviour belongs in `usbemu.c`, next to the
kernel code it copies. The tests pass on Wine 9.0 and 11.20.

Real hardware beats an emulator. `src/lsusb-wine.c` is the quickest
check that the library loads and lists your devices.
