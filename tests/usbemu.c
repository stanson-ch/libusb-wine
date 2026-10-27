/*
 * usbemu: Linux usbfs emulated in user space, for testing libusb0.dll.so
 * without hardware. It is preloaded (LD_PRELOAD) into Wine, intercepts
 * open(), close(), ioctl() and poll() on the device nodes of a fake
 * /dev/bus/usb tree, and behaves the way drivers/usb/core/devio.c does in
 * the ways the library depends on:
 *
 *  - one completion list per open file, reaped with REAPURB(NDELAY), and
 *    poll() reporting POLLOUT while it is not empty
 *  - DISCARDURB, which fails with EINVAL once the URB has finished
 *  - USBDEVFS_BULK, which waits for its transfer without going through
 *    the completion list
 *  - an interrupt URB refused on a bulk endpoint, a bulk URB accepted on
 *    an interrupt one
 *  - interfaces claimed by kernel drivers or by other files (EBUSY),
 *    DISCONNECT_CLAIM, and SETCONFIGURATION refused while any is claimed
 *  - every request refused (EPERM) on a file opened read-only
 *  - unplugging: pending URBs complete, poll() reports POLLHUP, reaps
 *    return ENODEV once nothing is left, everything else fails with ENODEV
 *
 * The devices, on bus 001:
 *   005  0fcf:1009 ANT USB-m stick that answers ANT requests (writable)
 *   006  046d:c52b receiver bound to usbhid, which must be left alone
 *   001  root hub (writable here, to check that hubs are not listed)
 *   007  a device this user cannot open for writing
 *
 * Built twice from this file: as the preload library, and with
 * -DUSBEMU_MKTREE as the tool that writes the device tree (the nodes are
 * plain files holding each device's descriptors).
 *
 * Environment:
 *   USBEMU_ROOT        the tree; point USB_DEVFS_PATH at it too
 *   USBEMU_LOG         append what the devices see to this file
 *   USBEMU_DRIVER      bind the ANT stick to this kernel driver
 *   USBEMU_TRAFFIC_HZ  unsolicited ANT broadcasts per second
 *   USBEMU_UNPLUG_MS   unplug the ANT stick this long after its first claim
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/usbdevice_fs.h>

#define LE16(x) ((x) & 0xff), (((x) >> 8) & 0xff)

enum { EP_BULK = 2, EP_INTERRUPT = 3 };

struct endpoint {
    unsigned char address, type;
};

struct device {
    const char *node;               /* bus/device under USBEMU_ROOT */
    int devnum;
    int writable;                   /* may this user open it read-write? */
    int is_ant;
    unsigned char descriptors[18 + 64];
    int descriptors_len;
    struct endpoint endpoints[2];
    int nendpoints;
    const char *strings[4];         /* string descriptors 1..3 */
    unsigned char ports[4];         /* a hub's ports: devnums, 0 = empty */
    /* state */
    char driver[64];                /* kernel driver on interface 0, "" none */
    struct file *claimed_by;        /* file holding interface 0, or NULL */
    int config;
    int unplugged;
    unsigned char rx[512];          /* ANT: bytes received, not yet parsed */
    int rx_len;
};

static struct device devices[] = {
    {
        "001/005", 5, 1, 1,
        { 18, 1, LE16(0x0200), 0, 0, 0, 32, LE16(0x0fcf), LE16(0x1009),
          LE16(0x0100), 1, 2, 3, 1,
          9, 2, LE16(32), 1, 1, 0, 0x80, 50,
          9, 4, 0, 0, 2, 0xff, 0, 0, 0,
          7, 5, 0x81, EP_BULK, LE16(64), 1,
          7, 5, 0x01, EP_BULK, LE16(64), 1 },
        18 + 32, { { 0x81, EP_BULK }, { 0x01, EP_BULK } }, 2,
        { NULL, "Dynastream Innovations", "ANT USBStick2", "123" },
        { 0 }, "", NULL, 1, 0, { 0 }, 0
    },
    {
        "001/006", 6, 1, 0,
        { 18, 1, LE16(0x0200), 0, 0, 0, 8, LE16(0x046d), LE16(0xc52b),
          LE16(0x1211), 1, 2, 0, 1,
          9, 2, LE16(25), 1, 1, 0, 0xa0, 49,
          9, 4, 0, 0, 1, 3, 1, 1, 0,
          7, 5, 0x81, EP_INTERRUPT, LE16(8), 8 },
        18 + 25, { { 0x81, EP_INTERRUPT } }, 1,
        { NULL, "Logitech", "USB Receiver", NULL },
        { 0 }, "usbhid", NULL, 1, 0, { 0 }, 0
    },
    {
        "001/001", 1, 1, 0,
        { 18, 1, LE16(0x0200), 9, 0, 1, 64, LE16(0x1d6b), LE16(0x0002),
          LE16(0x0608), 3, 2, 1, 1,
          9, 2, LE16(25), 1, 1, 0, 0xe0, 0,
          9, 4, 0, 0, 1, 9, 0, 0, 0,
          7, 5, 0x81, EP_INTERRUPT, LE16(4), 12 },
        18 + 25, { { 0x81, EP_INTERRUPT } }, 1,
        { NULL, "Linux", "xHCI Host Controller", "0000:00:14.0" },
        { 5, 6, 7, 0 }, "hub", NULL, 1, 0, { 0 }, 0
    },
    {
        "001/007", 7, 0, 0,
        { 18, 1, LE16(0x0200), 0xe0, 1, 1, 64, LE16(0x8087), LE16(0x0029),
          LE16(0x0001), 0, 0, 0, 1,
          9, 2, LE16(25), 1, 1, 0, 0xe0, 0,
          9, 4, 0, 0, 1, 0xe0, 1, 1, 0,
          7, 5, 0x81, EP_INTERRUPT, LE16(16), 1 },
        18 + 25, { { 0x81, EP_INTERRUPT } }, 1,
        { NULL, NULL, NULL, NULL },
        { 0 }, "btusb", NULL, 1, 0, { 0 }, 0
    },
};
#define NDEVICES ((int)(sizeof(devices) / sizeof(devices[0])))
#define ANT (&devices[0])

#ifdef USBEMU_MKTREE

int main(int argc, char **argv)
{
    char path[4096];
    int i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s DIR\n", argv[0]);
        return 2;
    }
    mkdir(argv[1], 0755);
    snprintf(path, sizeof(path), "%s/001", argv[1]);
    mkdir(path, 0755);
    for (i = 0; i < NDEVICES; i++) {
        FILE *f;

        snprintf(path, sizeof(path), "%s/%s", argv[1], devices[i].node);
        if (!(f = fopen(path, "wb"))) {
            perror(path);
            return 1;
        }
        fwrite(devices[i].descriptors, 1, devices[i].descriptors_len, f);
        fclose(f);
    }
    return 0;
}

#else

/* ---- state -------------------------------------------------------------- */

/* A request: an async URB, or a synchronous IN transfer waiting for data */
struct urb {
    struct file *file;
    struct usbdevfs_urb *user;      /* the program's URB, written at reap */
    unsigned char endpoint;
    void *user_buffer;
    unsigned char *data;            /* the "kernel" copy of the data */
    int length, actual, status;
    int sync, done;
    struct urb *next;               /* the file's pending or completed list */
    struct urb *queue_next;         /* the ANT stick's IN queue */
};

/* An open device file */
struct file {
    int fd;
    int writable;
    struct device *device;
    int ready;                      /* eventfd, readable while URBs wait */
    int ready_set;
    struct urb *pending;
    struct urb *completed, *completed_tail;
    struct file *next;
};

/* An ANT message on its way to the host */
struct message {
    unsigned char bytes[64];
    int len;
    struct timespec due;
    struct message *next;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct file *files;
static unsigned char tracked[65536];        /* fd -> is a device file */
static struct urb *in_queue, *in_queue_tail; /* the ANT stick's IN requests */
static struct message *outbox;               /* ANT messages, by due time */
static int device_thread_started;
static char root[1024];
static FILE *log_file;
static int traffic_hz, unplug_ms, unplug_armed;
static struct timespec unplug_at;

static int (*real_open)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_close)(int);
static int (*real_ioctl)(int, unsigned long, ...);
static int (*real_poll)(struct pollfd *, nfds_t, int);

static void log_event(const char *format, ...)
{
    va_list args;

    if (!log_file)
        return;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fputc('\n', log_file);
    fflush(log_file);
}

__attribute__((constructor)) static void init(void)
{
    const char *v;

    real_open = dlsym(RTLD_NEXT, "open");
    real_openat = dlsym(RTLD_NEXT, "openat");
    real_close = dlsym(RTLD_NEXT, "close");
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_poll = dlsym(RTLD_NEXT, "poll");
    if ((v = getenv("USBEMU_ROOT")))
        snprintf(root, sizeof(root), "%s", v);
    if ((v = getenv("USBEMU_LOG")))
        log_file = fopen(v, "a");
    if ((v = getenv("USBEMU_DRIVER")) && *v)
        snprintf(ANT->driver, sizeof(ANT->driver), "%s", v);
    if ((v = getenv("USBEMU_TRAFFIC_HZ")))
        traffic_hz = atoi(v);
    if ((v = getenv("USBEMU_UNPLUG_MS")))
        unplug_ms = atoi(v);
}

static void add_us(struct timespec *ts, long us)
{
    ts->tv_nsec += (us % 1000000) * 1000;
    ts->tv_sec += us / 1000000 + ts->tv_nsec / 1000000000;
    ts->tv_nsec %= 1000000000;
}

static int before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

static struct file *file_of(int fd)
{
    struct file *f;

    if (fd < 0 || fd >= 65536 || !tracked[fd])
        return NULL;
    for (f = files; f; f = f->next)
        if (f->fd == fd)
            return f;
    return NULL;
}

/* The eventfd stands in for the file in poll(): readable while URBs wait. */
static void update_ready(struct file *f)
{
    uint64_t v = 1;
    int want = f->completed || f->device->unplugged;

    if (want && !f->ready_set) {
        if (write(f->ready, &v, sizeof(v)) == sizeof(v))
            f->ready_set = 1;
    } else if (!want && f->ready_set) {
        if (read(f->ready, &v, sizeof(v)) == sizeof(v))
            f->ready_set = 0;
    }
}

static void unlink_from(struct urb **list, struct urb *u)
{
    for (; *list; list = &(*list)->next)
        if (*list == u) {
            *list = u->next;
            break;
        }
    u->next = NULL;
}

static void unqueue(struct urb *u)
{
    struct urb **p, *prev = NULL;

    for (p = &in_queue; *p; prev = *p, p = &(*p)->queue_next)
        if (*p == u) {
            *p = u->queue_next;
            if (in_queue_tail == u)
                in_queue_tail = prev;
            break;
        }
    u->queue_next = NULL;
}

static void enqueue(struct urb *u)
{
    if (in_queue_tail)
        in_queue_tail->queue_next = u;
    else
        in_queue = u;
    in_queue_tail = u;
}

/* A request finished: an async URB moves to its file's completed list. */
static void complete(struct urb *u, int status)
{
    struct file *f = u->file;

    u->status = status;
    u->done = 1;
    unqueue(u);
    if (!u->sync) {
        unlink_from(&f->pending, u);
        if (f->completed_tail)
            f->completed_tail->next = u;
        else
            f->completed = u;
        f->completed_tail = u;
        update_ready(f);
    }
    pthread_cond_broadcast(&changed);
}

/* ---- the ANT stick -------------------------------------------------------- */

static void send_message(unsigned char id, const unsigned char *data, int len, long delay_us)
{
    struct message *m = calloc(1, sizeof(*m)), **p;
    unsigned char sum = 0;
    int i;

    m->bytes[0] = 0xa4;
    m->bytes[1] = len;
    m->bytes[2] = id;
    memcpy(m->bytes + 3, data, len);
    for (i = 0; i < 3 + len; i++)
        sum ^= m->bytes[i];
    m->bytes[3 + len] = sum;
    m->len = 4 + len;
    clock_gettime(CLOCK_MONOTONIC, &m->due);
    add_us(&m->due, delay_us);
    for (p = &outbox; *p && !before(&m->due, &(*p)->due); p = &(*p)->next)
        ;
    m->next = *p;
    *p = m;
    pthread_cond_broadcast(&changed);
}

/* Bytes from the host: answer the requests ANT_DLL makes when it opens a stick. */
static void ant_receive(struct device *d, const unsigned char *bytes, int n)
{
    static const unsigned char startup[] = { 0x20 };
    static const unsigned char capabilities[] = { 8, 8, 0x00, 0xba, 0x36, 0x00, 0xdf, 0x0f };
    static const unsigned char serial[] = { 0x78, 0x56, 0x34, 0x12 };
    static const unsigned char version[] = "AOD1.10B00";

    if (d->rx_len + n > (int)sizeof(d->rx))
        d->rx_len = 0;
    memcpy(d->rx + d->rx_len, bytes, n);
    d->rx_len += n;

    for (;;) {
        int start = 0, len, i;
        unsigned char sum = 0;

        while (start < d->rx_len && d->rx[start] != 0xa4)
            start++;
        memmove(d->rx, d->rx + start, d->rx_len - start);
        d->rx_len -= start;
        if (d->rx_len < 4 || d->rx_len < 4 + (len = d->rx[1]))
            return;
        for (i = 0; i < 3 + len; i++)
            sum ^= d->rx[i];
        if (sum == d->rx[3 + len]) {
            unsigned char id = d->rx[2], *data = d->rx + 3;

            if (id == 0x4a)                                     /* reset */
                send_message(0x6f, startup, sizeof(startup), 2000);
            else if (id == 0x4d && len >= 2 && data[1] == 0x54) /* request */
                send_message(0x54, capabilities, sizeof(capabilities), 300);
            else if (id == 0x4d && len >= 2 && data[1] == 0x61)
                send_message(0x61, serial, sizeof(serial), 300);
            else if (id == 0x4d && len >= 2 && data[1] == 0x3e)
                send_message(0x3e, version, sizeof(version), 300);
        }
        memmove(d->rx, d->rx + 4 + len, d->rx_len - (4 + len));
        d->rx_len -= 4 + len;
    }
}

static void unplug(struct device *d)
{
    struct file *f;

    d->unplugged = 1;
    log_event("unplug %s", d->node);
    /* the kernel kills everything in flight, and it lands as completed */
    while (in_queue)
        complete(in_queue, in_queue->sync ? -ESHUTDOWN : -ENOENT);
    for (f = files; f; f = f->next)
        if (f->device == d) {
            while (f->pending)
                complete(f->pending, -ENOENT);
            update_ready(f);
        }
}

/* Hand due messages to waiting IN requests, one message per transfer. */
static void deliver(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (unplug_armed && !ANT->unplugged && !before(&now, &unplug_at)) {
        unplug(ANT);
        return;
    }
    while (outbox && in_queue && !ANT->unplugged && !before(&now, &outbox->due)) {
        struct message *m = outbox;
        struct urb *u = in_queue;
        int n = m->len < u->length ? m->len : u->length;

        outbox = m->next;
        memcpy(u->data, m->bytes, n);
        u->actual = n;
        complete(u, 0);
        free(m);
    }
}

static void *device_thread(void *unused)
{
    struct timespec next_broadcast;

    (void)unused;
    clock_gettime(CLOCK_MONOTONIC, &next_broadcast);
    pthread_mutex_lock(&lock);
    for (;;) {
        struct timespec now, wake, real;
        long ns;

        deliver();
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (traffic_hz > 0 && !before(&now, &next_broadcast)) {
            static unsigned char page[9];

            page[1]++;
            send_message(0x4e, page, sizeof(page), 0);  /* broadcast data, channel 0 */
            add_us(&next_broadcast, 1000000 / traffic_hz);
        }
        wake = now;
        add_us(&wake, 50000);
        if (outbox && before(&outbox->due, &wake))
            wake = outbox->due;
        if (traffic_hz > 0 && before(&next_broadcast, &wake))
            wake = next_broadcast;
        if (unplug_armed && before(&unplug_at, &wake))
            wake = unplug_at;

        /* the condition variable runs on CLOCK_REALTIME */
        ns = (wake.tv_sec - now.tv_sec) * 1000000000L + (wake.tv_nsec - now.tv_nsec);
        clock_gettime(CLOCK_REALTIME, &real);
        add_us(&real, ns > 0 ? ns / 1000 : 0);
        pthread_cond_timedwait(&changed, &lock, &real);
    }
    return NULL;
}

/* ---- usbfs ---------------------------------------------------------------- */

static struct device *device_at(const char *path)
{
    size_t n = strlen(root);
    int i;

    if (!root[0] || strncmp(path, root, n) || path[n] != '/')
        return NULL;
    for (i = 0; i < NDEVICES; i++)
        if (!strcmp(path + n + 1, devices[i].node))
            return &devices[i];
    return NULL;
}

static int endpoint_type(struct device *d, unsigned char address)
{
    int i;

    for (i = 0; i < d->nendpoints; i++)
        if (d->endpoints[i].address == address)
            return d->endpoints[i].type;
    return -1;
}

/* usbfs claims an unclaimed interface on first use (checkintf()) */
static int use_interface(struct file *f)
{
    struct device *d = f->device;

    if (d->claimed_by == f)
        return 0;
    if (d->claimed_by || d->driver[0])
        return -EBUSY;
    d->claimed_by = f;
    return 0;
}

static void release_interface(struct file *f)
{
    if (f->device->claimed_by != f)
        return;
    f->device->claimed_by = NULL;
    /* destroy_async_on_interface() */
    while (f->pending)
        complete(f->pending, -ENOENT);
}

static int string_descriptor(struct device *d, int index, unsigned char *out, int max)
{
    unsigned char b[256];
    int n, i;

    if (index == 0) {
        b[0] = 4; b[1] = 3; b[2] = 0x09; b[3] = 0x04;  /* English (US) */
        n = 4;
    } else if (index < 4 && d->strings[index]) {
        const char *s = d->strings[index];

        for (i = 0; s[i]; i++) {
            b[2 + 2 * i] = s[i];
            b[3 + 2 * i] = 0;
        }
        b[0] = n = 2 + 2 * i;
        b[1] = 3;
    } else
        return -EPIPE;
    if (n > max)
        n = max;
    memcpy(out, b, n);
    return n;
}

static int control(struct file *f, struct usbdevfs_ctrltransfer *c)
{
    struct device *d = f->device;
    unsigned char *data = c->data;
    int n;

    if (c->bRequestType == 0x80 && c->bRequest == 6) {         /* GET_DESCRIPTOR */
        switch (c->wValue >> 8) {
        case 1:
            n = c->wLength < 18 ? c->wLength : 18;
            memcpy(data, d->descriptors, n);
            return n;
        case 2:
            n = d->descriptors_len - 18;
            if (n > c->wLength)
                n = c->wLength;
            memcpy(data, d->descriptors + 18, n);
            return n;
        case 3:
            return string_descriptor(d, c->wValue & 0xff, data, c->wLength);
        }
        return -EPIPE;
    }
    if (c->bRequestType == 0x80 && c->bRequest == 8 && c->wLength >= 1) {  /* GET_CONFIGURATION */
        data[0] = d->config;
        return 1;
    }
    return -EPIPE;
}

/* USBDEVFS_BULK: waits for its own transfer, never on the completion list */
static int bulk(struct file *f, struct usbdevfs_bulktransfer *b)
{
    struct device *d = f->device;
    struct timespec deadline;
    struct urb u;
    int ret;

    if (endpoint_type(d, b->ep) < 0)
        return -ENOENT;
    if ((ret = use_interface(f)))
        return ret;
    if (!(b->ep & 0x80)) {
        log_event("bulk out ep %02x %u bytes", b->ep, b->len);
        if (d->is_ant)
            ant_receive(d, b->data, b->len);
        return b->len;
    }

    memset(&u, 0, sizeof(u));
    u.file = f;
    u.endpoint = b->ep;
    u.length = b->len;
    u.sync = 1;
    u.data = malloc(b->len ? b->len : 1);
    enqueue(&u);
    deliver();
    clock_gettime(CLOCK_REALTIME, &deadline);
    add_us(&deadline, (long)b->timeout * 1000);
    while (!u.done) {
        if (!b->timeout)
            pthread_cond_wait(&changed, &lock);
        else if (pthread_cond_timedwait(&changed, &lock, &deadline) == ETIMEDOUT && !u.done) {
            unqueue(&u);
            free(u.data);
            return -ETIMEDOUT;
        }
    }
    ret = u.status ? u.status : u.actual;
    if (ret > 0)
        memcpy(b->data, u.data, u.actual);
    free(u.data);
    return ret;
}

static int submit(struct file *f, struct usbdevfs_urb *uu)
{
    struct device *d = f->device;
    int type = endpoint_type(d, uu->endpoint), ret;
    struct urb *u;

    if (type < 0)
        return -ENOENT;
    if (uu->type == USBDEVFS_URB_TYPE_INTERRUPT && type != EP_INTERRUPT)
        return -EINVAL;
    if (uu->type == USBDEVFS_URB_TYPE_BULK && type != EP_BULK && type != EP_INTERRUPT)
        return -EINVAL;
    if (uu->buffer_length < 0)
        return -EINVAL;
    if ((ret = use_interface(f)))
        return ret;

    u = calloc(1, sizeof(*u));
    u->file = f;
    u->user = uu;
    u->endpoint = uu->endpoint;
    u->user_buffer = uu->buffer;
    u->length = uu->buffer_length;
    u->data = malloc(u->length ? u->length : 1);
    u->next = f->pending;
    f->pending = u;
    if (u->endpoint & 0x80) {
        enqueue(u);
        deliver();
    } else {
        memcpy(u->data, uu->buffer, u->length);
        if (d->is_ant)
            ant_receive(d, u->data, u->length);
        u->actual = u->length;
        complete(u, 0);
    }
    return 0;
}

static int reap(struct file *f, void **arg, int block)
{
    struct urb *u;

    while (!(u = f->completed)) {
        if (f->device->unplugged)
            return -ENODEV;
        if (!block)
            return -EAGAIN;
        pthread_cond_wait(&changed, &lock);
    }
    f->completed = u->next;
    if (!f->completed)
        f->completed_tail = NULL;
    update_ready(f);

    /* now, and only now, the kernel writes to the program's memory */
    if ((u->endpoint & 0x80) && u->actual > 0)
        memcpy(u->user_buffer, u->data, u->actual);
    u->user->status = u->status;
    u->user->actual_length = u->actual;
    *arg = u->user;
    free(u->data);
    free(u);
    return 0;
}

static int discard(struct file *f, void *user)
{
    struct urb *u;

    for (u = f->pending; u; u = u->next)
        if (u->user == user) {
            complete(u, -ENOENT);
            return 0;
        }
    return -EINVAL;
}

static int disconnect_claim(struct file *f, struct usbdevfs_disconnect_claim *dc)
{
    struct device *d = f->device;
    int ret;

    if (dc->interface != 0)
        return -EINVAL;
    if (d->claimed_by && d->claimed_by != f)
        return -EBUSY;          /* another file holds it: its driver is "usbfs" */
    if (d->driver[0]) {
        if ((dc->flags & USBDEVFS_DISCONNECT_CLAIM_EXCEPT_DRIVER) && !strcmp(dc->driver, d->driver))
            return -EBUSY;
        if ((dc->flags & USBDEVFS_DISCONNECT_CLAIM_IF_DRIVER) && strcmp(dc->driver, d->driver))
            return -EBUSY;
        log_event("detach %s from %s", d->driver, d->node);
        d->driver[0] = 0;
    }
    ret = use_interface(f);
    log_event("claim %s (disconnect-claim): %s", d->node, ret ? "EBUSY" : "ok");
    return ret;
}

static int usbfs_ioctl(struct file *f, unsigned long request, void *arg)
{
    struct device *d = f->device;
    int ret;

    if (!f->writable)
        return -EPERM;
    if (request == USBDEVFS_REAPURB || request == USBDEVFS_REAPURBNDELAY)
        return reap(f, arg, request == USBDEVFS_REAPURB);
    if (d->unplugged)
        return -ENODEV;

    switch (request) {
    case USBDEVFS_CONNECTINFO:
        ((struct usbdevfs_connectinfo *)arg)->devnum = d->devnum;
        ((struct usbdevfs_connectinfo *)arg)->slow = 0;
        return 0;
    case USBDEVFS_CONTROL:
        return control(f, arg);
    case USBDEVFS_BULK:
        return bulk(f, arg);
    case USBDEVFS_SUBMITURB:
        return submit(f, arg);
    case USBDEVFS_DISCARDURB:
        return discard(f, arg);
    case USBDEVFS_SETCONFIGURATION:
        if (d->claimed_by || d->driver[0]) {
            log_event("setconfig %d on %s: EBUSY", *(int *)arg, d->node);
            return -EBUSY;
        }
        d->config = *(int *)arg;
        log_event("setconfig %d on %s", d->config, d->node);
        return 0;
    case USBDEVFS_CLAIMINTERFACE:
        if (*(unsigned int *)arg != 0)
            return -EINVAL;
        ret = use_interface(f);
        log_event("claim %s: %s", d->node, ret ? "EBUSY" : "ok");
        if (!ret && d->is_ant && unplug_ms && !unplug_armed) {
            unplug_armed = 1;
            clock_gettime(CLOCK_MONOTONIC, &unplug_at);
            add_us(&unplug_at, (long)unplug_ms * 1000);
            pthread_cond_broadcast(&changed);
        }
        return ret;
    case USBDEVFS_RELEASEINTERFACE:
        if (*(unsigned int *)arg != 0 || d->claimed_by != f)
            return -EINVAL;
        release_interface(f);
        log_event("release %s", d->node);
        return 0;
    case USBDEVFS_DISCONNECT_CLAIM:
        return disconnect_claim(f, arg);
    case USBDEVFS_IOCTL: {
        struct usbdevfs_ioctl *io = arg;

        if (io->ioctl_code == USBDEVFS_DISCONNECT) {
            if (!d->driver[0])
                return -ENODATA;
            log_event("detach %s from %s", d->driver, d->node);
            d->driver[0] = 0;
            return 0;
        }
        if (io->ioctl_code == (int)USBDEVFS_HUB_PORTINFO) {
            struct usbdevfs_hub_portinfo *info = io->data;
            int i;

            log_event("portinfo %s", d->node);
            if (strcmp(d->driver, "hub"))
                return -ENOSYS;         /* not a hub driver's ioctl */
            memset(info, 0, sizeof(*info));
            info->nports = 4;
            for (i = 0; i < 4; i++)
                info->port[i] = d->ports[i];
            return info->nports;
        }
        return -ENOTTY;
    }
    case USBDEVFS_CLEAR_HALT:
    case USBDEVFS_RESETEP:
        return 0;
    case USBDEVFS_RESET:
        log_event("reset %s", d->node);
        return 0;
    }
    return -ENOTTY;
}

/* ---- the interposed C library ---------------------------------------------- */

static int track(const char *path, int flags, int fd)
{
    struct device *d;
    struct file *f;

    if (fd < 0 || fd >= 65536 || !(d = device_at(path)))
        return fd;
    f = calloc(1, sizeof(*f));
    f->fd = fd;
    f->device = d;
    f->writable = (flags & O_ACCMODE) != O_RDONLY;
    f->ready = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    pthread_mutex_lock(&lock);
    f->next = files;
    files = f;
    tracked[fd] = 1;
    if (!device_thread_started) {
        pthread_t t;

        device_thread_started = 1;
        pthread_create(&t, NULL, device_thread, NULL);
    }
    update_ready(f);
    pthread_mutex_unlock(&lock);
    return fd;
}

static int refused(const char *path, int flags)
{
    struct device *d = device_at(path);

    return d && (flags & O_ACCMODE) != O_RDONLY && !d->writable;
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list args;

        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    if (!real_open)
        init();
    if (refused(path, flags)) {
        errno = EACCES;
        return -1;
    }
    return track(path, flags, real_open(path, flags, mode));
}

int open64(const char *path, int flags, ...) __attribute__((alias("open")));
int __open_2(const char *path, int flags) { return open(path, flags); }
int __open64_2(const char *path, int flags) { return open(path, flags); }

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list args;

        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    if (!real_openat)
        init();
    if (path[0] != '/')
        return real_openat(dirfd, path, flags, mode);
    if (refused(path, flags)) {
        errno = EACCES;
        return -1;
    }
    return track(path, flags, real_openat(dirfd, path, flags, mode));
}

int close(int fd)
{
    if (!real_close)
        init();
    if (fd >= 0 && fd < 65536 && tracked[fd]) {
        struct file **p, *f;

        pthread_mutex_lock(&lock);
        for (p = &files; (f = *p); p = &f->next)
            if (f->fd == fd) {
                struct urb *u;

                *p = f->next;
                /* usbdev_release(): release the interface, free every URB */
                release_interface(f);
                while ((u = f->pending)) {
                    f->pending = u->next;
                    unqueue(u);
                    free(u->data);
                    free(u);
                }
                while ((u = f->completed)) {
                    f->completed = u->next;
                    free(u->data);
                    free(u);
                }
                real_close(f->ready);
                free(f);
                break;
            }
        tracked[fd] = 0;
        pthread_mutex_unlock(&lock);
    }
    return real_close(fd);
}

int ioctl(int fd, unsigned long request, ...)
{
    struct file *f;
    va_list args;
    void *arg;
    int ret;

    va_start(args, request);
    arg = va_arg(args, void *);
    va_end(args);
    if (!real_ioctl)
        init();
    if (fd < 0 || fd >= 65536 || !tracked[fd])
        return real_ioctl(fd, request, arg);

    pthread_mutex_lock(&lock);
    f = file_of(fd);
    ret = f ? usbfs_ioctl(f, request, arg) : -EBADF;
    pthread_mutex_unlock(&lock);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    return ret;
}

/* Each device file is swapped for its eventfd, then given usbfs's events. */
int poll(struct pollfd *fds, nfds_t n, int timeout)
{
    struct pollfd local[16];
    nfds_t i;
    int any = 0, ret;

    if (!real_poll)
        init();
    for (i = 0; i < n; i++)
        if (fds[i].fd >= 0 && fds[i].fd < 65536 && tracked[fds[i].fd])
            any = 1;
    if (!any || n > 16)
        return real_poll(fds, n, timeout);

    pthread_mutex_lock(&lock);
    for (i = 0; i < n; i++) {
        struct file *f = file_of(fds[i].fd);

        local[i] = fds[i];
        if (f) {
            local[i].fd = f->ready;
            local[i].events = POLLIN;
        }
    }
    pthread_mutex_unlock(&lock);

    ret = real_poll(local, n, timeout);
    if (ret < 0)
        return ret;

    pthread_mutex_lock(&lock);
    ret = 0;
    for (i = 0; i < n; i++) {
        struct file *f = file_of(fds[i].fd);

        if (f) {
            fds[i].revents = 0;
            if (f->completed && f->writable && (fds[i].events & POLLOUT))
                fds[i].revents |= POLLOUT;
            if (f->device->unplugged)
                fds[i].revents |= POLLHUP | POLLERR;
        } else
            fds[i].revents = local[i].revents;
        if (fds[i].revents)
            ret++;
    }
    pthread_mutex_unlock(&lock);
    return ret;
}

int __poll_chk(struct pollfd *fds, nfds_t n, int timeout, size_t len)
{
    (void)len;
    return poll(fds, n, timeout);
}

#endif /* USBEMU_MKTREE */
