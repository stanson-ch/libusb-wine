/*
 * libusb0-test.exe: libusb0.dll as a Windows program sees it.
 *
 * An ordinary Windows program, built with mingw and linked to libusb0.dll
 * through an import library. run.sh builds it and runs it under Wine with
 * the usbfs emulator (usbemu.c) preloaded. Each argument names a test:
 *
 *   abi       exports and the Microsoft x64 calling convention
 *   list      which devices are listed, and libusb-win32's virtual hub
 *   list-all  the same with LIBUSB0_ALL_DEVICES=1: every device, real tree
 *   claim     configurations, interfaces, taking a kernel driver's device
 *   keep      with LIBUSB0_DETACH_KERNEL_DRIVER=0: the driver keeps it
 *   sync      synchronous transfers
 *   async     asynchronous transfers: timeouts, cancel, free, last error
 *   threads   several threads at once, and closing under a waiting one
 *   unplug    the stick goes away under ANT_DLL's receive loop
 *   cycles    300 open/use/close cycles, counting file descriptors
 *   ant       ANT_DLL's own sequence, as Zwift runs it
 *
 * The device it talks to is the emulator's ANT USB-m stick: a request
 * goes out as an ANT message on the bulk OUT endpoint, and the reply comes
 * back on the bulk IN endpoint a fraction of a millisecond later.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* the Windows headers define interface as struct; libusb-win32's own
   header undefines it the same way */
#undef interface

/* usb.h uses the BSD names for its integer types, which mingw lacks */
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
#include "usb.h"

#define ETRANSFER_TIMEDOUT  116                 /* libusb-win32's */
#define ANT_VENDOR          0x0fcf
#define EP_IN               0x81
#define EP_OUT              0x01

static int failures;

#define CHECK(cond, ...) \
    do { \
        int ok_ = !!(cond); \
        printf(ok_ ? "  PASS  " : "  FAIL  "); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        failures += !ok_; \
    } while (0)
#define INFO(...) do { printf("        "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* GetTickCount() moves in 16 ms steps under Wine */
static DWORD ms_now(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;

    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (DWORD)(now.QuadPart * 1000 / frequency.QuadPart);
}

/* ANT request "send me message 0x54", the stick's capabilities */
static char caps_request[] = { (char)0xa4, 0x02, 0x4d, 0x00, 0x54, (char)0xbf };

static int is_caps_reply(const char *b, int n)
{
    return n >= 3 && (unsigned char)b[0] == 0xa4 && (unsigned char)b[2] == 0x54;
}

static struct usb_device *find(unsigned short vendor, unsigned short product)
{
    struct usb_bus *bus;
    struct usb_device *dev;

    usb_init();
    usb_find_busses();
    usb_find_devices();
    for (bus = usb_get_busses(); bus; bus = bus->next)
        for (dev = bus->devices; dev; dev = dev->next)
            if (dev->descriptor.idVendor == vendor &&
                (!product || dev->descriptor.idProduct == product))
                return dev;
    return NULL;
}

/* open, configure and claim, the way ANT_DLL does */
static usb_dev_handle *open_stick(void)
{
    struct usb_device *dev = find(ANT_VENDOR, 0);
    usb_dev_handle *h;

    if (!dev || !(h = usb_open(dev)))
        return NULL;
    if (usb_set_configuration(h, 1) || usb_claim_interface(h, 0)) {
        usb_close(h);
        return NULL;
    }
    return h;
}

/* ---- abi ------------------------------------------------------------------ */

/*
 * Call fn with chosen values in both conventions' argument registers:
 * rcx and rdx for Microsoft x64, which is what a Windows caller uses, and
 * rdi and rsi for System V, which is what a Linux-compiled function reads.
 * Whichever pair it acts on shows its convention. Also note whether rdi
 * and rsi survive: Microsoft x64 makes them callee-saved, System V doesn't.
 */
struct probe {
    void *fn;                                   /*  0 */
    unsigned long long ms1, ms2;                /*  8, 16 */
    unsigned long long sv1, sv2;                /* 24, 32 */
    unsigned long long ret;                     /* 40 */
    unsigned long long rdi_after, rsi_after;    /* 48, 56 */
};

static void call_probe(struct probe *p)
{
    __asm__ volatile(
        "mov  %%rsp, %%r12\n\t"
        "and  $-16, %%rsp\n\t"
        "sub  $64, %%rsp\n\t"                   /* shadow space, arguments 5..8 */
        "movq $0, 32(%%rsp)\n\t"
        "movq $0, 40(%%rsp)\n\t"
        "movq $0, 48(%%rsp)\n\t"
        "movq $0, 56(%%rsp)\n\t"
        "mov  8(%%rbx), %%rcx\n\t"
        "mov  16(%%rbx), %%rdx\n\t"
        "xor  %%r8d, %%r8d\n\t"
        "xor  %%r9d, %%r9d\n\t"
        "mov  24(%%rbx), %%rdi\n\t"
        "mov  32(%%rbx), %%rsi\n\t"
        "call *(%%rbx)\n\t"
        "mov  %%r12, %%rsp\n\t"
        "mov  %%rax, 40(%%rbx)\n\t"
        "mov  %%rdi, 48(%%rbx)\n\t"
        "mov  %%rsi, 56(%%rbx)\n\t"
        :
        : "b"(p)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12",
          "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
          "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
          "xmm14", "xmm15");
}

static unsigned long long probe(void *fn, unsigned long long ms1, unsigned long long ms2,
                                unsigned long long sv1, unsigned long long sv2, int *kept)
{
    struct probe p = { fn, ms1, ms2, sv1, sv2, 0, 0, 0 };

    call_probe(&p);
    *kept = p.rdi_after == sv1 && p.rsi_after == sv2;
    return p.ret;
}

static void test_abi(void)
{
    static const char *const exports[] = {
        "usb_open", "usb_close", "usb_get_string", "usb_get_string_simple",
        "usb_get_descriptor_by_endpoint", "usb_get_descriptor", "usb_bulk_write",
        "usb_bulk_read", "usb_interrupt_write", "usb_interrupt_read",
        "usb_control_msg", "usb_set_configuration", "usb_claim_interface",
        "usb_release_interface", "usb_set_altinterface", "usb_resetep",
        "usb_clear_halt", "usb_reset", "usb_strerror", "usb_init", "usb_set_debug",
        "usb_find_busses", "usb_find_devices", "usb_device", "usb_get_busses",
        "usb_get_version", "usb_bulk_setup_async", "usb_interrupt_setup_async",
        "usb_submit_async", "usb_reap_async", "usb_reap_async_nocancel",
        "usb_cancel_async", "usb_free_async",
    };
    static struct usb_bus bus_ms, bus_sv;
    static struct usb_device dev_ms, dev_sv;
    HMODULE m = GetModuleHandleA("libusb0.dll");
    unsigned i, found = 0, clobbered = 0;
    const char *msg;
    int kept;

    for (i = 0; i < sizeof(exports) / sizeof(exports[0]); i++)
        if (GetProcAddress(m, exports[i]))
            found++;
        else
            INFO("missing: %s", exports[i]);
    CHECK(found == sizeof(exports) / sizeof(exports[0]),
          "%u of %u libusb-win32 functions exported", found,
          (unsigned)(sizeof(exports) / sizeof(exports[0])));

    /* usb_open() on a device that does not exist names it in the error */
    strcpy(bus_ms.dirname, "rcx-bus"); strcpy(dev_ms.filename, "rcx-dev"); dev_ms.bus = &bus_ms;
    strcpy(bus_sv.dirname, "rdi-bus"); strcpy(dev_sv.filename, "rdi-dev"); dev_sv.bus = &bus_sv;
    usb_init();
    probe((void *)GetProcAddress(m, "usb_open"),
          (ULONG_PTR)&dev_ms, 0, (ULONG_PTR)&dev_sv, 0, &kept);
    msg = usb_strerror();
    CHECK(strstr(msg, "rcx-bus/rcx-dev") && kept,
          "usb_open() takes its argument from rcx and keeps rdi and rsi (%s)", msg);

    /* the second argument: rdx, not rsi */
    probe((void *)GetProcAddress(m, "usb_set_configuration"), 0, 77, 0, 55, &kept);
    msg = usb_strerror();
    CHECK(strstr(msg, "config 77") && kept,
          "usb_set_configuration() takes its second argument from rdx (%s)", msg);

    /* every export, with poison in rdi and rsi: a System V function would
       crash on it or clobber it */
    for (i = 0; i < sizeof(exports) / sizeof(exports[0]); i++) {
        void *fn = (void *)GetProcAddress(m, exports[i]);

        if (!fn)
            continue;
        probe(fn, 0, 0, 0x5151515151515151ULL, 0x7d7d7d7d7d7d7d7dULL, &kept);
        if (!kept) {
            INFO("%s clobbers rdi or rsi", exports[i]);
            clobbered++;
        }
    }
    CHECK(!clobbered, "every export keeps rdi and rsi, and none crashes on NULL arguments");
}

/* ---- list ----------------------------------------------------------------- */

static void test_list(int all)
{
    struct usb_bus *bus;
    struct usb_device *dev, *root = NULL;
    int listed = 0, ant = 0, receiver = 0, hub = 0, locked = 0, children = 0, i;

    usb_init();
    CHECK(usb_find_busses() >= 0 && usb_find_devices() >= 0, "enumeration succeeds");
    for (bus = usb_get_busses(); bus; bus = bus->next) {
        for (dev = bus->devices; dev; dev = dev->next) {
            unsigned short v = dev->descriptor.idVendor;

            INFO("%s/%s %04x:%04x", bus->dirname, dev->filename, v, dev->descriptor.idProduct);
            listed++;
            ant += v == ANT_VENDOR;
            receiver += v == 0x046d;
            hub += v == 0x1d6b;
            locked += v == 0x8087;
        }
        if (bus->root_dev)
            root = bus->root_dev;
    }

    if (!all) {
        CHECK(ant == 1 && receiver == 1 && listed == 2,
              "listed: the two devices this user may write to (%d listed)", listed);
        CHECK(!hub && !locked, "not listed: the hub and the read-only device");
        CHECK(root && root->descriptor.bDeviceClass == USB_CLASS_HUB &&
              !strcmp(root->filename, "virtual-hub"),
              "the bus has libusb-win32's virtual hub as its root");
        if (root)
            for (i = 0; i < root->num_children; i++)
                children += root->children[i] != NULL;
        CHECK(root && root->num_children == listed && children == listed,
              "every listed device hangs off it (%d of %d)", children, listed);
    } else {
        CHECK(listed == 4, "LIBUSB0_ALL_DEVICES=1: all four devices listed");
        CHECK(root && root->descriptor.idVendor == 0x1d6b,
              "the real hub is the root");
        if (root)
            for (i = 0; i < root->num_children; i++)
                children += root->children[i] != NULL;
        CHECK(root && root->num_children == 3 && children == 3,
              "with the other three devices as its children (%d)", root ? root->num_children : -1);
    }
    CHECK(usb_find_busses() == 0 && usb_find_devices() == 0,
          "enumerating again reports no changes");
}

/* ---- claim ---------------------------------------------------------------- */

static void test_claim(void)
{
    static struct usb_bus nobus;
    static struct usb_device nodev;
    struct usb_device *dev;
    usb_dev_handle *h = NULL;

    dev = find(ANT_VENDOR, 0);
    CHECK(dev && (h = usb_open(dev)), "open the ANT stick");
    if (!dev || !h)
        return;
    CHECK(usb_set_configuration(h, 1) == 0,
          "asking for the configuration it is in succeeds (and changes nothing)");
    CHECK(usb_claim_interface(h, 0) == 0,
          "claim interface 0, which a kernel driver has: it is taken over");
    CHECK(usb_claim_interface(h, 0) == 0, "claiming it again succeeds");
    CHECK(usb_set_configuration(h, 2) == -EINVAL,
          "changing the configuration with an interface claimed: -EINVAL");
    CHECK(usb_release_interface(h, 0) == 0, "release it");
    CHECK(usb_close(h) == 0, "close");

    dev = find(0x046d, 0);
    CHECK(dev && (h = usb_open(dev)), "open the receiver bound to usbhid");
    if (dev && h) {
        CHECK(usb_set_configuration(h, 1) == 0, "set its configuration: nothing to do");
        CHECK(usb_claim_interface(h, 0) == -EIO,
              "claiming it fails: only listed devices are taken from their driver");
        usb_close(h);
    }

    strcpy(nobus.dirname, "001");
    strcpy(nodev.filename, "099");
    nodev.bus = &nobus;
    CHECK(usb_open(&nodev) == NULL, "usb_open() of a device that is not there: NULL");
    CHECK(usb_open(NULL) == NULL && usb_close(NULL) == -EINVAL &&
          usb_claim_interface(NULL, 0) == -EINVAL && usb_device(NULL) == NULL,
          "NULL handles are refused, not dereferenced");
}

/* with LIBUSB0_DETACH_KERNEL_DRIVER=0 */
static void test_keep(void)
{
    struct usb_device *dev = find(ANT_VENDOR, 0);
    usb_dev_handle *h = NULL;

    CHECK(dev && (h = usb_open(dev)), "open the ANT stick");
    if (!dev || !h)
        return;
    CHECK(usb_claim_interface(h, 0) == -EIO,
          "LIBUSB0_DETACH_KERNEL_DRIVER=0: its kernel driver keeps it");
    usb_close(h);
}

/* ---- sync ----------------------------------------------------------------- */

static void test_sync(void)
{
    char buf[64];
    usb_dev_handle *h = open_stick();
    DWORD t0, dt;
    int ret;

    CHECK(h != NULL, "open, configure, claim");
    if (!h)
        return;

    t0 = ms_now();
    ret = usb_bulk_read(h, EP_IN, buf, sizeof(buf), 30);
    dt = ms_now() - t0;
    CHECK(ret == -ETRANSFER_TIMEDOUT && dt >= 25 && dt < 500,
          "a read with nothing to read times out: %d after %lu ms", ret, dt);

    ret = usb_interrupt_write(h, EP_OUT, caps_request, sizeof(caps_request), 1000);
    CHECK(ret == sizeof(caps_request),
          "usb_interrupt_write() to a bulk endpoint, as ANT_DLL does it: %d", ret);
    ret = usb_bulk_read(h, EP_IN, buf, sizeof(buf), 1000);
    CHECK(is_caps_reply(buf, ret), "usb_bulk_read() gets the reply: %d bytes", ret);

    usb_bulk_write(h, EP_OUT, caps_request, sizeof(caps_request), 1000);
    ret = usb_interrupt_read(h, EP_IN, buf, sizeof(buf), 1000);
    CHECK(is_caps_reply(buf, ret), "usb_interrupt_read() from a bulk endpoint: %d bytes", ret);

    usb_bulk_write(h, EP_OUT, caps_request, sizeof(caps_request), 1000);
    ret = usb_bulk_read(h, EP_IN, buf, sizeof(buf), 0);
    CHECK(is_caps_reply(buf, ret), "a timeout of 0 waits until the data comes: %d bytes", ret);

    usb_close(h);
}

/* ---- async ---------------------------------------------------------------- */

static void test_async(void)
{
    static char buf[4096];
    usb_dev_handle *h = open_stick();
    void *ctx = NULL;
    DWORD t0, dt, err;
    int ret;

    CHECK(h != NULL, "open, configure, claim");
    if (!h)
        return;
    CHECK(usb_bulk_setup_async(h, &ctx, EP_IN) == 0 && ctx, "set up a read");
    CHECK(usb_submit_async(ctx, buf, sizeof(buf)) == 0, "submit it");
    CHECK(usb_submit_async(ctx, buf, sizeof(buf)) == -EINVAL,
          "submitting it again while it is pending: -EINVAL");

    SetLastError(0);
    t0 = ms_now();
    ret = usb_reap_async_nocancel(ctx, 0);
    dt = ms_now() - t0;
    err = GetLastError();
    CHECK(ret == -ETRANSFER_TIMEDOUT && dt < 50 && err == 121,
          "timeout 0 checks without waiting: %d after %lu ms, last error %lu", ret, dt, err);

    t0 = ms_now();
    ret = usb_reap_async_nocancel(ctx, 100);
    dt = ms_now() - t0;
    CHECK(ret == -ETRANSFER_TIMEDOUT && dt >= 90 && dt < 500,
          "a 100 ms reap times out after %lu ms and leaves the request running", dt);

    usb_bulk_write(h, EP_OUT, caps_request, sizeof(caps_request), 1000);
    ret = usb_reap_async_nocancel(ctx, 1000);
    CHECK(is_caps_reply(buf, ret), "the same request then gets the reply: %d bytes", ret);
    CHECK(usb_reap_async_nocancel(ctx, 0) == ret,
          "reaping a finished request again gives the same result");

    usb_submit_async(ctx, buf, sizeof(buf));
    t0 = ms_now();
    ret = usb_reap_async(ctx, 50);
    dt = ms_now() - t0;
    CHECK(ret == -ETRANSFER_TIMEDOUT && dt >= 40 && dt < 500,
          "usb_reap_async() times out after %lu ms", dt);
    ret = usb_reap_async_nocancel(ctx, 0);
    err = GetLastError();
    CHECK(ret == -ETRANSFER_TIMEDOUT && err == ERROR_OPERATION_ABORTED,
          "and has cancelled it: %d, last error %lu", ret, err);

    usb_submit_async(ctx, buf, sizeof(buf));
    CHECK(usb_cancel_async(ctx) == 0, "usb_cancel_async()");
    t0 = ms_now();
    ret = usb_reap_async_nocancel(ctx, 1000);
    dt = ms_now() - t0;
    err = GetLastError();
    CHECK(ret == -ETRANSFER_TIMEDOUT && err == ERROR_OPERATION_ABORTED && dt < 50,
          "a cancelled request reaps at once as aborted: %d, last error %lu", ret, err);

    usb_submit_async(ctx, buf, sizeof(buf));
    CHECK(usb_free_async(&ctx) == 0 && ctx == NULL, "free a context with its request pending");
    usb_bulk_setup_async(h, &ctx, EP_IN);
    usb_submit_async(ctx, buf, sizeof(buf));
    usb_bulk_write(h, EP_OUT, caps_request, sizeof(caps_request), 1000);
    ret = usb_reap_async_nocancel(ctx, 1000);
    CHECK(is_caps_reply(buf, ret), "the freed request did not take the next reply: %d", ret);

    usb_free_async(&ctx);
    usb_release_interface(h, 0);
    usb_bulk_setup_async(h, &ctx, EP_IN);
    CHECK(usb_submit_async(ctx, buf, sizeof(buf)) == -EINVAL,
          "submitting with no interface claimed: -EINVAL, as in libusb-win32");
    usb_free_async(&ctx);
    usb_close(h);
}

/* ---- threads -------------------------------------------------------------- */

struct reaper {
    void *ctx;
    int timeout, ret;
    DWORD error, ms;
    char buf[4096];
};

static DWORD WINAPI reaper_thread(LPVOID arg)
{
    struct reaper *r = arg;
    DWORD t0 = ms_now();

    r->ret = usb_reap_async_nocancel(r->ctx, r->timeout);
    r->error = GetLastError();
    r->ms = ms_now() - t0;
    return 0;
}

static void test_threads(void)
{
    static struct reaper a, b;
    usb_dev_handle *h = open_stick();
    HANDLE ta, tb;
    DWORD t0, dt;
    int ret;

    CHECK(h != NULL, "open, configure, claim");
    if (!h)
        return;

    usb_bulk_setup_async(h, &a.ctx, EP_IN);
    usb_bulk_setup_async(h, &b.ctx, EP_IN);
    a.timeout = b.timeout = 2000;
    usb_submit_async(a.ctx, a.buf, sizeof(a.buf));
    usb_submit_async(b.ctx, b.buf, sizeof(b.buf));
    ta = CreateThread(NULL, 0, reaper_thread, &a, 0, NULL);
    tb = CreateThread(NULL, 0, reaper_thread, &b, 0, NULL);
    Sleep(50);
    usb_interrupt_write(h, EP_OUT, caps_request, sizeof(caps_request), 1000);
    usb_interrupt_write(h, EP_OUT, caps_request, sizeof(caps_request), 1000);
    WaitForSingleObject(ta, 3000);
    WaitForSingleObject(tb, 3000);
    CHECK(is_caps_reply(a.buf, a.ret) && is_caps_reply(b.buf, b.ret) && a.ms < 500 && b.ms < 500,
          "two threads reaping at once each get their reply (%lu ms, %lu ms)", a.ms, b.ms);

    /* a write on this thread while the other keeps a read pending */
    usb_submit_async(a.ctx, a.buf, sizeof(a.buf));
    a.timeout = 2000;
    ta = CreateThread(NULL, 0, reaper_thread, &a, 0, NULL);
    Sleep(20);
    ret = usb_bulk_write(h, EP_OUT, caps_request, sizeof(caps_request), 1000);
    WaitForSingleObject(ta, 3000);
    CHECK(ret == sizeof(caps_request) && is_caps_reply(a.buf, a.ret),
          "a write while another thread reaps: the reply goes to the reaper");

    usb_submit_async(a.ctx, a.buf, sizeof(a.buf));
    a.timeout = -1;
    ta = CreateThread(NULL, 0, reaper_thread, &a, 0, NULL);
    Sleep(100);
    t0 = ms_now();
    usb_close(h);
    dt = ms_now() - t0;
    ret = WaitForSingleObject(ta, 2000);
    CHECK(ret == WAIT_OBJECT_0 && a.ret == -ETRANSFER_TIMEDOUT &&
          a.error == ERROR_OPERATION_ABORTED && dt < 500,
          "usb_close() ends a reap that waits for ever, as aborted (close took %lu ms)", dt);
    CHECK(usb_free_async(&a.ctx) == 0 && usb_free_async(&b.ctx) == 0,
          "the contexts can still be freed after the close");
}

/* ---- unplug --------------------------------------------------------------- */

static usb_dev_handle *loop_handle;
static volatile LONG loop_stop, loop_aborts, loop_errors, loop_reads;
static DWORD loop_end;

/* ANT_DLL's receive loop, USBDeviceHandleLibusb::ReceiveThread() */
static DWORD WINAPI receive_loop(LPVOID unused)
{
    static char data[4096];
    void *ctx = NULL;
    int submitted = 0, errors_in_a_row = 0, ret;

    (void)unused;
    usb_bulk_setup_async(loop_handle, &ctx, EP_IN);
    while (!loop_stop) {
        if (!submitted) {
            if (usb_submit_async(ctx, data, sizeof(data)) < 0) {
                loop_errors++;
                break;
            }
            submitted = 1;
        }
        ret = usb_reap_async_nocancel(ctx, 1000);
        if (ret > 0) {
            loop_reads++;
            submitted = 0;
            errors_in_a_row = 0;
        } else if (ret == -ETRANSFER_TIMEDOUT) {
            if (GetLastError() == ERROR_OPERATION_ABORTED) {
                loop_aborts++;                  /* "the stick lost power" */
                break;
            }
        } else if (ret < 0) {
            loop_errors++;
            if (++errors_in_a_row == 10)
                break;
            if (usb_cancel_async(ctx) || usb_free_async(&ctx) ||
                usb_bulk_setup_async(loop_handle, &ctx, EP_IN))
                break;
            submitted = 0;
        }
    }
    loop_end = ms_now();
    usb_cancel_async(ctx);
    usb_free_async(&ctx);
    return 0;
}

/* the emulator pulls the stick out 1.5 s after the claim */
static void test_unplug(void)
{
    HANDLE t;
    DWORD t0;
    int ret;

    loop_handle = open_stick();
    CHECK(loop_handle != NULL, "open, configure, claim");
    if (!loop_handle)
        return;
    t0 = ms_now();
    t = CreateThread(NULL, 0, receive_loop, NULL, 0, NULL);
    while (ms_now() - t0 < 1300) {
        usb_interrupt_write(loop_handle, EP_OUT, caps_request, sizeof(caps_request), 1000);
        Sleep(20);
    }
    CHECK(WaitForSingleObject(t, 2500) == WAIT_OBJECT_0, "the receive loop ends after the unplug");
    CHECK(loop_aborts == 1 && loop_errors == 0 && loop_reads > 0,
          "with the abort ANT_DLL restarts a stick on (%ld aborts, %ld errors, %ld reads)",
          loop_aborts, loop_errors, loop_reads);
    CHECK(loop_end - t0 < 1500 + 400, "within %ld ms of the unplug", (long)(loop_end - t0) - 1500);
    ret = usb_interrupt_write(loop_handle, EP_OUT, caps_request, sizeof(caps_request), 1000);
    CHECK(ret == -EIO, "writing to the missing stick fails at once: %d", ret);
    CHECK(usb_close(loop_handle) == 0, "close it");
}

/* ---- cycles --------------------------------------------------------------- */

/* open file descriptors of this (Wine) process, through Z:, which is / */
static int count_fds(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA("Z:\\proc\\self\\fd\\*", &fd);
    int n = 0;

    if (f == INVALID_HANDLE_VALUE)
        return -1;
    do
        n++;
    while (FindNextFileA(f, &fd));
    FindClose(f);
    return n;
}

static void test_cycles(void)
{
    static char buf[4096];
    int i, before, after, with_data = 0;
    usb_dev_handle *h;
    void *ctx;

    h = open_stick();                   /* first use allocates for good */
    if (h)
        usb_close(h);
    before = count_fds();
    for (i = 0; i < 300; i++) {
        if (!(h = open_stick()))
            break;
        ctx = NULL;
        usb_bulk_setup_async(h, &ctx, EP_IN);
        usb_submit_async(ctx, buf, sizeof(buf));
        if (i % 3 == 0) {
            usb_interrupt_write(h, EP_OUT, caps_request, sizeof(caps_request), 1000);
            with_data += usb_reap_async_nocancel(ctx, 1000) > 0;
        } else if (i % 3 == 1)
            usb_cancel_async(ctx);
        usb_free_async(&ctx);           /* every third one still pending */
        usb_close(h);
    }
    after = count_fds();
    CHECK(i == 300 && with_data == 100, "300 open/use/close cycles, 100 of them with data");
    CHECK(before > 0 && after <= before, "no file descriptor leaked: %d before, %d after",
          before, after);
}

/* ---- ant ------------------------------------------------------------------ */

/* USBDeviceHandleLibusb::TryOpen(): ANT_DLL runs it on every listed device */
static int try_open(struct usb_device *dev)
{
    usb_dev_handle *h = usb_open(dev);
    int ok;

    if (!h)
        return 0;
    ok = usb_set_configuration(h, 1) == 0 && usb_claim_interface(h, 0) == 0;
    if (ok)
        usb_release_interface(h, 0);
    usb_close(h);
    return ok;
}

static void test_ant(void)
{
    struct usb_bus *bus;
    struct usb_device *dev, *stick = NULL;
    char name[64];
    int openable = 0, i, ok = 0;
    HANDLE t;
    DWORD t0, worst = 0;

    /* USBDeviceHandleLibusb::GetAllDevices() and GetAvailableDevices() */
    usb_init();
    usb_find_busses();
    usb_find_devices();
    for (bus = usb_get_busses(); bus; bus = bus->next)
        for (dev = bus->devices; dev; dev = dev->next) {
            usb_dev_handle *h = usb_open(dev);

            if (h) {        /* USBDeviceLibusb reads the strings of every device */
                usb_get_string_simple(h, dev->descriptor.iProduct, name, sizeof(name));
                usb_close(h);
            }
            if (try_open(dev)) {
                openable++;
                if (dev->descriptor.idVendor == ANT_VENDOR)
                    stick = dev;
            }
        }
    CHECK(stick && openable == 1, "trial-opening every device finds the stick, and only it");
    if (!stick)
        return;

    /* POpen(), then the receive thread, then requests from this thread */
    loop_handle = usb_open(stick);
    CHECK(loop_handle && usb_set_configuration(loop_handle, 1) == 0 &&
          usb_claim_interface(loop_handle, 0) == 0, "open the stick the way POpen() does");
    if (!loop_handle)
        return;
    loop_stop = loop_aborts = loop_errors = loop_reads = 0;
    t = CreateThread(NULL, 0, receive_loop, NULL, 0, NULL);
    for (i = 0; i < 200; i++) {
        LONG before = loop_reads;
        DWORD t1 = ms_now();

        if (usb_interrupt_write(loop_handle, EP_OUT, caps_request, sizeof(caps_request), 3000) != 6)
            break;
        while (loop_reads == before && ms_now() - t1 < 1000)
            Sleep(0);
        if (loop_reads != before) {
            ok++;
            if (ms_now() - t1 > worst)
                worst = ms_now() - t1;
        }
    }
    CHECK(ok == 200, "200 request/reply round trips through the receive thread (worst %lu ms)",
          worst);

    /* PClose(): stop the thread, which ANT_DLL kills if it takes 3 s */
    t0 = ms_now();
    loop_stop = 1;
    CHECK(WaitForSingleObject(t, 3000) == WAIT_OBJECT_0 && loop_errors == 0,
          "the receive thread stops in %lu ms, without errors", ms_now() - t0);
    usb_release_interface(loop_handle, 0);
    usb_close(loop_handle);

    CHECK(try_open(stick), "and the stick opens again afterwards");
}

/* ---- main ----------------------------------------------------------------- */

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*run)(void); } tests[] = {
        { "abi", test_abi }, { "claim", test_claim }, { "keep", test_keep },
        { "sync", test_sync }, { "async", test_async }, { "threads", test_threads },
        { "unplug", test_unplug }, { "cycles", test_cycles }, { "ant", test_ant },
    };
    int i, j;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: libusb0-test TEST...\n");
        return 2;
    }
    for (i = 1; i < argc; i++) {
        printf("%s\n", argv[i]);
        if (!strcmp(argv[i], "list") || !strcmp(argv[i], "list-all")) {
            test_list(argv[i][4] != 0);
            continue;
        }
        for (j = 0; j < (int)(sizeof(tests) / sizeof(tests[0])); j++)
            if (!strcmp(argv[i], tests[j].name))
                break;
        if (j == (int)(sizeof(tests) / sizeof(tests[0]))) {
            fprintf(stderr, "no test called %s\n", argv[i]);
            return 2;
        }
        tests[j].run();
    }
    return failures ? 1 : 0;
}
