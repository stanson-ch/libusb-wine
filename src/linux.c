/*
 * Linux USB support
 *
 * Copyright (c) 2000-2003 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include <stdlib.h>	/* getenv, etc */
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <dirent.h>
#include <sys/eventfd.h>
#include <sys/sysmacros.h>
#include <linux/usbdevice_fs.h>

#include "linux.h"
#include "usbi.h"
#include "usb-wine.h"

/* linux.h declares the kernel's usbfs structures itself: check them */
_Static_assert(sizeof(struct usb_urb) == sizeof(struct usbdevfs_urb), "usb_urb");
_Static_assert(sizeof(struct usb_bulktransfer) == sizeof(struct usbdevfs_bulktransfer),
	"usb_bulktransfer");
_Static_assert(IOCTL_USB_SUBMITURB == USBDEVFS_SUBMITURB, "IOCTL_USB_SUBMITURB");
_Static_assert(IOCTL_USB_DISCARDURB == USBDEVFS_DISCARDURB, "IOCTL_USB_DISCARDURB");
_Static_assert(IOCTL_USB_REAPURBNDELAY == USBDEVFS_REAPURBNDELAY, "IOCTL_USB_REAPURBNDELAY");
_Static_assert(IOCTL_USB_BULK == USBDEVFS_BULK, "IOCTL_USB_BULK");
_Static_assert(IOCTL_USB_DISCONNECT_CLAIM == USBDEVFS_DISCONNECT_CLAIM,
	"IOCTL_USB_DISCONNECT_CLAIM");

/*
 * libusb-win32's timeout error (from its error.h), the value Windows
 * programs compare against. Linux's own ETIMEDOUT is 110.
 */
#define ETRANSFER_TIMEDOUT 116

#define TRACE(level, format, args...) \
	do { \
	  if (usb_debug >= (level)) \
	    fprintf(stderr, "libusb0: " format "\n", ## args); \
	} while (0)

static char usb_path[PATH_MAX + 1] = "";

/* LIBUSB0_ALL_DEVICES=1: list every device, not only the usable ones */
static int list_all_devices;

/*
 * libusb-win32 sees no real hubs. It makes one up for each bus and hangs
 * every device it lists off that (usb_os_determine_children() in its
 * windows.c), and so does this when it lists only the usable devices. A
 * bus is allocated together with its virtual hub, so freeing the bus
 * frees both.
 */
#define MAX_DEVICES_PER_BUS	128	/* USB addresses go up to 127 */

struct linux_bus {
  struct usb_bus bus;		/* first: the rest of the library sees only this */
  struct usb_device hub;
  struct usb_device *children[MAX_DEVICES_PER_BUS];
};

/* per-device state for the asynchronous API, at the end of this file */
static int async_open(usb_dev_handle *dev);
static int async_close(usb_dev_handle *dev);

/*
 * libusb-win32 reports every failure as one of four errors
 * (usb_win_error_to_errno() in its error.c): EINVAL, ETRANSFER_TIMEDOUT,
 * ENOMEM and EIO. Windows programs were written against that set, so
 * Linux's errors are folded into it.
 */
static int win_error(int err)
{
  switch (err) {
  case EINVAL:
    return -EINVAL;
  case ENOMEM:
    return -ENOMEM;
  case ETIMEDOUT:
    return -ETRANSFER_TIMEDOUT;
  default:
    return -EIO;
  }
}

/* Timeouts run on CLOCK_MONOTONIC, so changing the clock cannot stretch them. */
static void deadline_in(struct timespec *ts, int ms)
{
  clock_gettime(CLOCK_MONOTONIC, ts);
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

/* Milliseconds left until ts, rounded up so that a wait never ends early;
   0 once ts has passed. */
static int ms_left(const struct timespec *ts)
{
  struct timespec now;
  long long ns;

  clock_gettime(CLOCK_MONOTONIC, &now);
  ns = (long long)(ts->tv_sec - now.tv_sec) * 1000000000LL
     + (ts->tv_nsec - now.tv_nsec);
  if (ns <= 0)
    return 0;
  ns = (ns + 999999) / 1000000;
  return ns > INT_MAX ? INT_MAX : (int)ns;
}

static int device_open(struct usb_device *dev)
{
  char filename[PATH_MAX + 1];
  int fd;

  snprintf(filename, sizeof(filename) - 1, "%s/%s/%s",
    usb_path, dev->bus->dirname, dev->filename);

  fd = open(filename, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      USB_ERROR_STR(-errno, "failed to open %s: %s",
	filename, strerror(errno));
  }

  return fd;
}

/*
 * The configuration the device is in, from the value the kernel keeps in
 * sysfs: no bus traffic, just as libusb-win32 asks its driver for the
 * cached value. Anything that is not a usbfs device node is asked with a
 * GET_CONFIGURATION request instead. 0 when unconfigured or unknown, which
 * is what libusb-win32 falls back to as well.
 */
static int active_configuration(int fd)
{
  struct usb_ctrltransfer ctrl;
  unsigned char value = 0;
  struct stat st;
  char path[64], buf[16];
  int f, n;

  if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/bConfigurationValue",
      major(st.st_rdev), minor(st.st_rdev));
    f = open(path, O_RDONLY | O_CLOEXEC);
    if (f >= 0) {
      n = read(f, buf, sizeof(buf) - 1);
      close(f);
      if (n >= 0) {
        buf[n] = 0;
        return atoi(buf);
      }
    }
  }

  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.bRequestType = USB_ENDPOINT_IN;
  ctrl.bRequest = USB_REQ_GET_CONFIGURATION;
  ctrl.wLength = 1;
  ctrl.timeout = 1000;
  ctrl.data = &value;
  if (ioctl(fd, IOCTL_USB_CONTROL, &ctrl) == 1)
    return value;

  return 0;
}

int usb_os_open(usb_dev_handle *dev)
{
  int ret;

  /* libusb-win32's usb_open() fails when the device cannot be opened,
     instead of handing out a handle on which every call fails */
  dev->fd = device_open(dev->device);
  if (dev->fd < 0)
    return dev->fd;

  ret = async_open(dev);
  if (ret < 0) {
    close(dev->fd);
    dev->fd = -1;
    return ret;
  }

  /* start in the configuration the device is in, as libusb-win32 does,
     so that asking for that one changes nothing */
  dev->config = active_configuration(dev->fd);

  return 0;
}

int usb_os_close(usb_dev_handle *dev)
{
  if (dev->fd < 0)
    return 0;

  /* libusb-win32 releases the claimed interface on close */
  if (dev->interface >= 0)
    usb_release_interface(dev, dev->interface);

  /* if a thread cannot be got out of usb_reap_async(), the file stays
     open for it: see async_close() */
  if (async_close(dev) < 0)
    return 0;

  if (close(dev->fd) == -1)
    /* Failing trying to close a file really isn't an error, so return 0 */
    USB_ERROR_STR(0, "tried to close device fd %d: %s", dev->fd,
	strerror(errno));

  return 0;
}

LIBUSB_API int usb_set_configuration(usb_dev_handle *dev, int configuration)
{
  int ret;

  if (!dev)
    USB_ERROR_STR(-EINVAL, "could not set config %d: invalid device handle",
      configuration);

  /*
   * As in libusb-win32: asking for the configuration the device is already
   * in does nothing, and no interface may be claimed. Linux would reset
   * the device's endpoints in the first case, and refuses with EBUSY while
   * a kernel driver holds an interface. The old code here detached the
   * kernel driver from interface 0 first, on any device a program merely
   * tried to open.
   */
  if (dev->config == configuration)
    return 0;
  if (dev->interface >= 0)
    USB_ERROR_STR(-EINVAL, "could not set config %d: an interface is claimed",
      configuration);

  ret = ioctl(dev->fd, IOCTL_USB_SETCONFIG, &configuration);
  if (ret < 0)
    USB_ERROR_STR(win_error(errno), "could not set config %d: %s", configuration,
	strerror(errno));

  dev->config = configuration;
  dev->interface = -1;
  dev->altsetting = -1;

  return 0;
}

/*
 * Devices that Linux gives a kernel driver and Windows does not: there,
 * the libusb0 driver has them to itself. Claiming an interface of one of
 * these takes it from its kernel driver, but never from another program
 * that has it through usbfs.
 *
 * The list is short on purpose, because programs probe: ANT_DLL, which
 * Zwift and other fitness programs use, claims interface 0 of every device
 * it is shown just to see whether it can. LIBUSB0_DETACH_KERNEL_DRIVER=1
 * extends the takeover to every device, and =0 turns it off.
 */
static const struct {
  unsigned short vendor, product;
} take_from_kernel[] = {
  { 0x0fcf, 0x1008 },	/* Dynastream ANT USB2 stick: usb_serial_simple */
  { 0x0fcf, 0x1009 },	/* Dynastream ANT USB-m stick: usb_serial_simple */
};

static int may_take_from_kernel(struct usb_device *dev)
{
  const char *env = getenv("LIBUSB0_DETACH_KERNEL_DRIVER");
  size_t i;

  if (env && *env)
    return atoi(env) != 0;
  for (i = 0; i < sizeof(take_from_kernel) / sizeof(take_from_kernel[0]); i++)
    if (dev->descriptor.idVendor == take_from_kernel[i].vendor &&
        dev->descriptor.idProduct == take_from_kernel[i].product)
      return 1;
  return 0;
}

LIBUSB_API int usb_claim_interface(usb_dev_handle *dev, int interface)
{
  struct usb_disconnect_claim dc;
  int ret, err;

  if (!dev)
    USB_ERROR_STR(-EINVAL, "could not claim interface %d: invalid device handle",
      interface);

  /* the checks libusb-win32 makes */
  if (!dev->config)
    USB_ERROR_STR(-EINVAL, "could not claim interface %d: device not configured",
      interface);
  if (dev->interface == interface)
    return 0;

  ret = ioctl(dev->fd, IOCTL_USB_CLAIMINTF, &interface);
  err = errno;
  if (ret < 0 && err == EBUSY && may_take_from_kernel(dev->device)) {
    memset(&dc, 0, sizeof(dc));
    dc.interface = interface;
    dc.flags = USB_DISCONNECT_CLAIM_EXCEPT_DRIVER;
    strcpy(dc.driver, "usbfs");
    ret = ioctl(dev->fd, IOCTL_USB_DISCONNECT_CLAIM, &dc);
    err = errno;
    if (ret == 0)
      TRACE(1, "took interface %d of %s/%s from its kernel driver", interface,
            dev->bus->dirname, dev->device->filename);
  }
  if (ret < 0) {
    if (err == EBUSY)
      TRACE(1, "interface %d of %s/%s is in use by a kernel driver or another program",
            interface, dev->bus->dirname, dev->device->filename);
    USB_ERROR_STR(win_error(err), "could not claim interface %d: %s", interface,
	strerror(err));
  }

  dev->interface = interface;
  dev->altsetting = 0;

  return 0;
}

LIBUSB_API int usb_release_interface(usb_dev_handle *dev, int interface)
{
  int ret;

  if (!dev)
    USB_ERROR_STR(-EINVAL, "could not release intf %d: invalid device handle",
      interface);

  ret = ioctl(dev->fd, IOCTL_USB_RELEASEINTF, &interface);
  if (ret < 0)
    USB_ERROR_STR(win_error(errno), "could not release intf %d: %s", interface,
    	strerror(errno));

  dev->interface = -1;
  dev->altsetting = -1;

  return 0;
}

LIBUSB_API int usb_set_altinterface(usb_dev_handle *dev, int alternate)
{
  int ret;
  struct usb_setinterface setintf;

  if (!dev)
    USB_ERROR_STR(-EINVAL, "could not set alt intf %d: invalid device handle",
      alternate);

  if (dev->interface < 0)
    USB_ERROR(-EINVAL);

  setintf.interface = dev->interface;
  setintf.altsetting = alternate;

  ret = ioctl(dev->fd, IOCTL_USB_SETINTF, &setintf);
  if (ret < 0)
    USB_ERROR_STR(win_error(errno), "could not set alt intf %d/%d: %s",
	dev->interface, alternate, strerror(errno));

  dev->altsetting = alternate;

  return 0;
}

/*
 * Older kernels refuse a synchronous transfer of more than 16 KB, so
 * bigger ones go in pieces.
 */
#define MAX_READ_WRITE	(16 * 1024)

LIBUSB_API int usb_control_msg(usb_dev_handle *dev, int requesttype, int request,
	int value, int index, char *bytes, int size, int timeout)
{
  struct usb_ctrltransfer ctrl;
  int ret;

  if (!dev)
    USB_ERROR_STR(-EINVAL, "error sending control message: invalid device handle");

  ctrl.bRequestType = requesttype;
  ctrl.bRequest = request;
  ctrl.wValue = value;
  ctrl.wIndex = index;
  ctrl.wLength = size;

  ctrl.data = bytes;
  ctrl.timeout = timeout;

  ret = ioctl(dev->fd, IOCTL_USB_CONTROL, &ctrl);
  if (ret < 0)
    USB_ERROR_STR(win_error(errno), "error sending control message: %s",
	strerror(errno));

  return ret;
}

/*
 * The four synchronous transfers. libusb-win32 does them all alike,
 * whatever the type of the endpoint, and so does the kernel's blocking
 * USBDEVFS_BULK, which picks bulk or interrupt from the endpoint. The old
 * code here submitted interrupt URBs for usb_interrupt_read/write(), which
 * the kernel refuses on bulk endpoints.
 *
 * USBDEVFS_BULK also stays out of the URB completion queue, so it can
 * neither take nor lose a completion that belongs to an asynchronous
 * request another thread has pending. The old code reaped from that
 * shared queue, which broke programs that keep a read pending while they
 * write.
 *
 * As in libusb-win32, a timeout of 0 (or less) means no timeout.
 */
static int usb_sync_transfer(usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout)
{
  struct usb_bulktransfer bulk;
  struct timespec deadline;
  int done = 0, requested, ret;

  if (!dev)
    USB_ERROR_STR(-EINVAL, "transfer on ep 0x%02x: invalid device handle", ep);

  if (timeout > 0)
    deadline_in(&deadline, timeout);

  do {
    requested = size - done;
    if (requested > MAX_READ_WRITE)
      requested = MAX_READ_WRITE;

    bulk.ep = ep;
    bulk.len = requested;
    bulk.data = bytes + done;
    bulk.timeout = 0;			/* the kernel's "no timeout" */
    if (timeout > 0) {
      bulk.timeout = ms_left(&deadline);
      if (!bulk.timeout)
        USB_ERROR_STR(-ETRANSFER_TIMEDOUT, "transfer on ep 0x%02x timed out", ep);
    }

    ret = ioctl(dev->fd, IOCTL_USB_BULK, &bulk);
    if (ret < 0)
      USB_ERROR_STR(win_error(errno), "transfer on ep 0x%02x failed: %s", ep,
	strerror(errno));

    done += ret;
  } while (ret == requested && done < size);

  return done;
}

LIBUSB_API int usb_bulk_write(usb_dev_handle *dev, int ep, char *bytes, int size,
	int timeout)
{
  /* Ensure the endpoint address is correct */
  return usb_sync_transfer(dev, ep, bytes, size, timeout);
}

LIBUSB_API int usb_bulk_read(usb_dev_handle *dev, int ep, char *bytes, int size,
	int timeout)
{
  /* Ensure the endpoint address is correct */
  ep |= USB_ENDPOINT_IN;
  return usb_sync_transfer(dev, ep, bytes, size, timeout);
}

LIBUSB_API int usb_interrupt_write(usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout)
{
  /* Ensure the endpoint address is correct */
  return usb_sync_transfer(dev, ep, bytes, size, timeout);
}

LIBUSB_API int usb_interrupt_read(usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout)
{
  /* Ensure the endpoint address is correct */
  ep |= USB_ENDPOINT_IN;
  return usb_sync_transfer(dev, ep, bytes, size, timeout);
}

int usb_os_find_busses(struct usb_bus **busses)
{
  struct usb_bus *fbus = NULL;
  DIR *dir;
  struct dirent *entry;

  /* no USB at all, as in a container: no busses, which is no error */
  if (!usb_path[0]) {
    *busses = NULL;
    return 0;
  }

  dir = opendir(usb_path);
  if (!dir)
    USB_ERROR_STR(-errno, "couldn't opendir(%s): %s", usb_path,
	strerror(errno));

  while ((entry = readdir(dir)) != NULL) {
    struct linux_bus *lbus;
    struct usb_bus *bus;

    /* Skip anything starting with a . */
    if (entry->d_name[0] == '.')
      continue;

    if (!strchr("0123456789", entry->d_name[strlen(entry->d_name) - 1])) {
      if (usb_debug >= 2)
        fprintf(stderr, "usb_os_find_busses: Skipping non bus directory %s\n",
		entry->d_name);
      continue;
    }

    lbus = calloc(1, sizeof(*lbus));
    if (!lbus) {
      closedir(dir);
      USB_ERROR(-ENOMEM);
    }
    bus = &lbus->bus;

    strncpy(bus->dirname, entry->d_name, sizeof(bus->dirname) - 1);
    bus->dirname[sizeof(bus->dirname) - 1] = 0;

    LIST_ADD(fbus, bus);

    if (usb_debug >= 2)
       fprintf(stderr, "usb_os_find_busses: Found %s\n", bus->dirname);
  }

  closedir(dir);

  *busses = fbus;

  return 0;
}

int usb_os_find_devices(struct usb_bus *bus, struct usb_device **devices)
{
  struct usb_device *fdev = NULL;
  DIR *dir;
  struct dirent *entry;
  char dirpath[PATH_MAX + 1];

  snprintf(dirpath, PATH_MAX, "%s/%s", usb_path, bus->dirname);

  dir = opendir(dirpath);
  if (!dir)
    USB_ERROR_STR(-errno, "couldn't opendir(%s): %s", dirpath,
	strerror(errno));

  while ((entry = readdir(dir)) != NULL) {
    unsigned char device_desc[DEVICE_DESC_LENGTH];
    char filename[PATH_MAX + 1];
    struct usb_device *dev;
    struct usb_connectinfo connectinfo;
    int i, fd, ret, writable;

    /* Skip anything starting with a . */
    if (entry->d_name[0] == '.')
      continue;

    dev = malloc(sizeof(*dev));
    if (!dev) {
      closedir(dir);
      USB_ERROR(-ENOMEM);
    }

    memset((void *)dev, 0, sizeof(*dev));

    dev->bus = bus;

    strncpy(dev->filename, entry->d_name, sizeof(dev->filename) - 1);
    dev->filename[sizeof(dev->filename) - 1] = 0;

    snprintf(filename, sizeof(filename) - 1, "%s/%s", dirpath, entry->d_name);
    fd = open(filename, O_RDWR | O_CLOEXEC);
    writable = fd >= 0;
    if (fd < 0) {
      fd = open(filename, O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        if (usb_debug >= 2)
          fprintf(stderr, "usb_os_find_devices: Couldn't open %s\n",
                  filename);

        free(dev);
        continue;
      }
    }

    /* Get the device number */
    ret = ioctl(fd, IOCTL_USB_CONNECTINFO, &connectinfo);
    if (ret < 0) {
      if (usb_debug)
        fprintf(stderr, "usb_os_find_devices: couldn't get connect info\n");
      /* refused on a read-only file; usbfs names the node after it */
      dev->devnum = atoi(entry->d_name);
    } else
      dev->devnum = connectinfo.devnum;

    ret = read(fd, (void *)device_desc, DEVICE_DESC_LENGTH);
    if (ret < DEVICE_DESC_LENGTH) {
      if (usb_debug)
        fprintf(stderr, "usb_os_find_devices: Couldn't read descriptor\n");

      free(dev);

      goto err;
    }

    /*
     * Linux kernel converts the words in this descriptor to CPU endian, so
     * we use the undocumented W character for usb_parse_descriptor() that
     * doesn't convert endianess when parsing the descriptor
     */
    usb_parse_descriptor(device_desc, "bbWbbbbWWWbbbb", &dev->descriptor);

    /*
     * libusb-win32 lists only the devices bound to its driver, the ones a
     * program can use. List the same kind here: devices this user may open
     * for writing (usbfs refuses every request on a read-only file), and
     * no hubs. Programs probe what they are shown -- ANT_DLL opens,
     * configures and claims every device -- and this keeps them off the
     * rest. LIBUSB0_ALL_DEVICES=1 lists everything.
     */
    if (!list_all_devices &&
        (!writable || dev->descriptor.bDeviceClass == USB_CLASS_HUB)) {
      if (usb_debug >= 2)
        fprintf(stderr, "usb_os_find_devices: Skipping %s on %s (%04x:%04x, %s)\n",
		dev->filename, bus->dirname, dev->descriptor.idVendor,
		dev->descriptor.idProduct, writable ? "a hub" : "not writable");
      free(dev);
      goto err;
    }

    LIST_ADD(fdev, dev);

    if (usb_debug >= 2)
      fprintf(stderr, "usb_os_find_devices: Found %s on %s\n",
		dev->filename, bus->dirname);

    /* Now try to fetch the rest of the descriptors */
    if (dev->descriptor.bNumConfigurations > USB_MAXCONFIG)
      /* Silent since we'll try again later */
      goto err;

    if (dev->descriptor.bNumConfigurations < 1)
      /* Silent since we'll try again later */
      goto err;

    dev->config = (struct usb_config_descriptor *)malloc(dev->descriptor.bNumConfigurations * sizeof(struct usb_config_descriptor));
    if (!dev->config)
      /* Silent since we'll try again later */
      goto err;

    memset(dev->config, 0, dev->descriptor.bNumConfigurations *
          sizeof(struct usb_config_descriptor));

    for (i = 0; i < dev->descriptor.bNumConfigurations; i++) {
      unsigned char buffer[8], *bigbuffer;
      struct usb_config_descriptor config;

      /* Get the first 8 bytes so we can figure out what the total length is */
      ret = read(fd, (void *)buffer, 8);
      if (ret < 8) {
        if (usb_debug >= 1) {
          if (ret < 0)
            fprintf(stderr, "Unable to get descriptor (%d)\n", ret);
          else
            fprintf(stderr, "Config descriptor too short (expected %d, got %d)\n", 8, ret);
        }

        goto err;
      }

      usb_parse_descriptor(buffer, "bbw", &config);

      /* a shorter one would make the copy below overrun bigbuffer */
      if (config.wTotalLength < 8) {
        if (usb_debug >= 1)
          fprintf(stderr, "Config descriptor too short (wTotalLength %d)\n",
                  config.wTotalLength);
        goto err;
      }

      bigbuffer = malloc(config.wTotalLength);
      if (!bigbuffer) {
        if (usb_debug >= 1)
          fprintf(stderr, "Unable to allocate memory for descriptors\n");
        goto err;
      }

      /* Read the rest of the config descriptor */
      memcpy(bigbuffer, buffer, 8);

      ret = read(fd, (void *)(bigbuffer + 8), config.wTotalLength - 8);
      if (ret < config.wTotalLength - 8) {
        if (usb_debug >= 1) {
          if (ret < 0)
            fprintf(stderr, "Unable to get descriptor (%d)\n", ret);
          else
            fprintf(stderr, "Config descriptor too short (expected %d, got %d)\n", config.wTotalLength, ret);
        }

        free(bigbuffer);
        goto err;
      }

      ret = usb_parse_configuration(&dev->config[i], bigbuffer);
      if (usb_debug >= 2) {
        if (ret > 0)
          fprintf(stderr, "Descriptor data still left\n");
        else if (ret < 0)
          fprintf(stderr, "Unable to parse descriptors\n");
      }

      free(bigbuffer);
    }

err:
    close(fd);
  }

  closedir(dir);

  *devices = fdev;

  return 0;
}

int usb_os_determine_children(struct usb_bus *bus)
{
  struct usb_device *dev, *devices[256];
  struct usb_ioctl command;
  int ret, i, i1;

  if (!list_all_devices) {
    struct linux_bus *lbus = (struct linux_bus *)bus;
    struct usb_device *hub = &lbus->hub;

    /* the device descriptor libusb-win32 gives its virtual hub */
    strcpy(hub->filename, "virtual-hub");
    hub->bus = bus;
    hub->descriptor.bLength = USB_DT_DEVICE_SIZE;
    hub->descriptor.bDescriptorType = USB_DT_DEVICE;
    hub->descriptor.bcdUSB = 0x0200;
    hub->descriptor.bDeviceClass = USB_CLASS_HUB;
    hub->descriptor.bMaxPacketSize0 = 64;
    hub->descriptor.bcdDevice = 0x0100;

    hub->children = lbus->children;
    hub->num_children = 0;
    for (dev = bus->devices; dev && hub->num_children < MAX_DEVICES_PER_BUS;
         dev = dev->next)
      hub->children[hub->num_children++] = dev;

    bus->root_dev = hub;
    return 0;
  }

  /* Create a list of devices first */
  memset(devices, 0, sizeof(devices));
  for (dev = bus->devices; dev; dev = dev->next)
    if (dev->devnum)
      devices[dev->devnum] = dev;

  /* Now fetch the children for each device */
  for (dev = bus->devices; dev; dev = dev->next) {
    struct usb_hub_portinfo portinfo;
    int fd;

    /* only hubs have children: leave the other devices alone */
    if (dev->descriptor.bDeviceClass != USB_CLASS_HUB)
      continue;

    fd = device_open(dev);
    if (fd < 0)
      continue;

    /* Query the hub driver for the children of this device */
    if (dev->config && dev->config->interface && dev->config->interface->altsetting)
      command.ifno = dev->config->interface->altsetting->bInterfaceNumber;
    else
      command.ifno = 0;
    command.ioctl_code = IOCTL_USB_HUB_PORTINFO;
    command.data = &portinfo;
    ret = ioctl(fd, IOCTL_USB_IOCTL, &command);
    if (ret < 0) {
      /* errno == ENOSYS means the device probably wasn't a hub */
      if (errno != ENOSYS && usb_debug > 1)
        fprintf(stderr, "error obtaining child information: %s\n",
		strerror(errno));

      close(fd);
      continue;
    }

    /* ports whose device is not listed are left out */
    dev->num_children = 0;
    for (i = 0; i < portinfo.numports; i++)
      if (portinfo.port[i] && devices[portinfo.port[i]])
        dev->num_children++;

    /* Free any old children first */
    free(dev->children);

    dev->children = malloc(sizeof(struct usb_device *) * dev->num_children);
    if (!dev->children) {
      if (usb_debug > 1)
        fprintf(stderr, "error allocating %zu bytes memory for dev->children\n",
                sizeof(struct usb_device *) * dev->num_children);

      dev->num_children = 0;
      close(fd);
      continue;
    }

    for (i = 0, i1 = 0; i < portinfo.numports; i++) {
      if (!portinfo.port[i] || !devices[portinfo.port[i]])
        continue;

      dev->children[i1++] = devices[portinfo.port[i]];

      devices[portinfo.port[i]] = NULL;
    }

    close(fd);
  }

  /*
   * There should be one device left in the devices list and that should be
   * the root device
   */
  for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
    if (devices[i])
      bus->root_dev = devices[i];
  }

  return 0;
}

static int check_usb_vfs(const char *dirname)
{
  DIR *dir;
  struct dirent *entry;
  int found = 0;

  dir = opendir(dirname);
  if (!dir)
    return 0;

  while ((entry = readdir(dir)) != NULL) {
    /* Skip anything starting with a . */
    if (entry->d_name[0] == '.')
      continue;

    /* We assume if we find any files that it must be the right place */
    found = 1;
    break;
  }

  closedir(dir);

  return found;
}

void usb_os_init(void)
{
  const char *all = getenv("LIBUSB0_ALL_DEVICES");

  list_all_devices = all && atoi(all) != 0;

  /* Find the path to the virtual filesystem */
  if (getenv("USB_DEVFS_PATH")) {
    if (check_usb_vfs(getenv("USB_DEVFS_PATH"))) {
      strncpy(usb_path, getenv("USB_DEVFS_PATH"), sizeof(usb_path) - 1);
      usb_path[sizeof(usb_path) - 1] = 0;
    } else if (usb_debug)
      fprintf(stderr, "usb_os_init: couldn't find USB VFS in USB_DEVFS_PATH\n");
  }

  if (!usb_path[0]) {
    if (check_usb_vfs("/dev/bus/usb")) {
      strncpy(usb_path, "/dev/bus/usb", sizeof(usb_path) - 1);
      usb_path[sizeof(usb_path) - 1] = 0;
    } else if (check_usb_vfs("/proc/bus/usb")) {
      strncpy(usb_path, "/proc/bus/usb", sizeof(usb_path) - 1);
      usb_path[sizeof(usb_path) - 1] = 0;
    } else
      usb_path[0] = 0;	/* No path, no USB support */
  }

  if (usb_debug) {
    if (usb_path[0])
      fprintf(stderr, "usb_os_init: Found USB VFS at %s\n", usb_path);
    else
      fprintf(stderr, "usb_os_init: No USB VFS found, is it mounted?\n");
  }
}

LIBUSB_API int usb_resetep(usb_dev_handle *dev, unsigned int ep)
{
  int ret;

  if (!dev)
    USB_ERROR_STR(-EINVAL, "could not reset ep %d: invalid device handle", ep);

  ret = ioctl(dev->fd, IOCTL_USB_RESETEP, &ep);
  if (ret)
    USB_ERROR_STR(win_error(errno), "could not reset ep %d: %s", ep,
    	strerror(errno));

  return 0;
}

LIBUSB_API int usb_clear_halt(usb_dev_handle *dev, unsigned int ep)
{
  int ret;

  if (!dev)
    USB_ERROR_STR(-EINVAL, "could not clear/halt ep %d: invalid device handle", ep);

  ret = ioctl(dev->fd, IOCTL_USB_CLEAR_HALT, &ep);
  if (ret)
    USB_ERROR_STR(win_error(errno), "could not clear/halt ep %d: %s", ep,
    	strerror(errno));

  return 0;
}

LIBUSB_API int usb_reset(usb_dev_handle *dev)
{
  int ret;

  if (!dev)
    USB_ERROR_STR(-EINVAL, "could not reset: invalid device handle");

  ret = ioctl(dev->fd, IOCTL_USB_RESET, NULL);
  if (ret)
     USB_ERROR_STR(win_error(errno), "could not reset: %s", strerror(errno));

  return 0;
}

LIBUSB_API int usb_get_driver_np(usb_dev_handle *dev, int interface, char *name,
	unsigned int namelen)
{
  struct usb_getdriver getdrv;
  int ret;

  getdrv.interface = interface;
  ret = ioctl(dev->fd, IOCTL_USB_GETDRIVER, &getdrv);
  if (ret)
    USB_ERROR_STR(-errno, "could not get bound driver: %s", strerror(errno));

  strncpy(name, getdrv.driver, namelen - 1);
  name[namelen - 1] = 0;

  return 0;
}

LIBUSB_API int usb_detach_kernel_driver_np(usb_dev_handle *dev, int interface)
{
  struct usb_ioctl command;
  int ret;

  command.ifno = interface;
  command.ioctl_code = IOCTL_USB_DISCONNECT;
  command.data = NULL;

  ret = ioctl(dev->fd, IOCTL_USB_IOCTL, &command);
  if (ret)
    USB_ERROR_STR(-errno, "could not detach kernel driver from interface %d: %s",
        interface, strerror(errno));

  return 0;
}

/* ------------------------------------------------------------------------
 * Asynchronous transfers
 *
 * libusb-win32 builds its asynchronous API on overlapped I/O:
 * usb_submit_async() starts a driver request, usb_reap_async() waits for
 * its event with WaitForSingleObject() and collects the result with
 * GetOverlappedResult(). What follows gives Windows programs the same
 * behaviour:
 *
 * - The timeout is WaitForSingleObject()'s: 0 checks without waiting, and
 *   a negative one (INFINITE) waits for as long as it takes. When it runs
 *   out, usb_reap_async() cancels the request and usb_reap_async_nocancel()
 *   leaves it running; both return -ETRANSFER_TIMEDOUT.
 * - A finished request stays finished: reaping it again gives the same
 *   result, until the context is submitted again.
 * - usb_cancel_async() aborts every request on the context's endpoint, as
 *   libusb-win32's ABORT_ENDPOINT does, and returns once they have
 *   stopped. They then reap as -ETRANSFER_TIMEDOUT.
 * - A failed reap leaves a Win32 error for GetLastError(), as
 *   GetOverlappedResult() would: ERROR_OPERATION_ABORTED for a request
 *   that was cancelled or lost to an unplug, ERROR_GEN_FAILURE when the
 *   transfer itself failed. After a plain timeout Windows leaves whatever
 *   error was there before; this sets ERROR_SEM_TIMEOUT instead, because a
 *   stale ERROR_OPERATION_ABORTED makes ANT_DLL think the stick is gone.
 * - usb_close() ends whatever is still pending as aborted, as closing the
 *   file handle does on Windows, and wakes the threads waiting on it.
 *
 * usbfs keeps one completion queue per open file: whoever reaps gets the
 * next finished URB, whichever thread submitted it. So all reaping is done
 * by drain_locked(), under the device's lock, which files every result
 * with the request it belongs to. Threads waiting for a result take turns
 * to sleep in poll() on the device; see wait_for().
 */

struct usb_async;

/* Per-device state, in usb_dev_handle.impl_info */
struct linux_handle {
  int fd;			/* the device, as in dev->fd */
  pthread_mutex_t lock;		/* guards all of this and every request */
  pthread_cond_t cond;		/* broadcast when results are filed, when
				   the thread in poll() leaves it, on close */
  int wake_fd;			/* eventfd that wakes the thread in poll() */
  int polling;			/* a thread is in poll() on fd */
  int waiters;			/* threads inside wait_for() */
  int gone;			/* the device has been unplugged */
  int closing;			/* usb_close() is under way; nobody reaps */
  struct usb_async *requests;	/* every context set up on the device */
};

enum { ASYNC_IDLE, ASYNC_PENDING, ASYNC_DONE };

/* What a libusb-win32 context points to */
struct usb_async {
  struct usb_urb urb;		/* the kernel names a finished URB by address */
  usb_dev_handle *dev;		/* NULL once the device is closed */
  struct usb_async *next;
  unsigned char ep;
  int state;
  int result;			/* bytes transferred, or a negative error */
  unsigned int last_error;	/* for GetLastError() when result < 0 */
  int orphaned;			/* freed by the program while still pending */
};

static struct linux_handle *handle_of(usb_dev_handle *dev)
{
  return dev ? dev->impl_info : NULL;
}

static void unlink_request(struct linux_handle *h, struct usb_async *c)
{
  struct usb_async **p;

  for (p = &h->requests; *p; p = &(*p)->next)
    if (*p == c) {
      *p = c->next;
      break;
    }
  c->next = NULL;
}

static void finish(struct usb_async *c, int result, unsigned int last_error)
{
  c->state = ASYNC_DONE;
  c->result = result;
  c->last_error = last_error;
}

/* Wake every thread waiting on the device, the one in poll() included. */
static void wake_locked(struct linux_handle *h)
{
  static const uint64_t one = 1;

  pthread_cond_broadcast(&h->cond);
  if (h->polling && write(h->wake_fd, &one, sizeof(one)) < 0) {
    /* the count is already non-zero: the poller is waking up anyway */
  }
}

/*
 * File a finished URB the way libusb-win32 reports a finished request.
 * Data that arrived is delivered, even when the request was then cut
 * short. A request that was cancelled, or lost when the device went away,
 * fails on Windows with ERROR_OPERATION_ABORTED, which libusb-win32
 * returns as -ETRANSFER_TIMEDOUT; ANT_DLL checks for exactly that pair.
 */
static void complete_locked(struct linux_handle *h, struct usb_async *c)
{
  int status = c->urb.status, length = c->urb.actual_length;

  if (status == 0 || length > 0)
    finish(c, length, 0);
  else if (status == -ENOENT || status == -ECONNRESET ||
           status == -ESHUTDOWN || status == -ENODEV)
    finish(c, -ETRANSFER_TIMEDOUT, USBWINE_ERROR_OPERATION_ABORTED);
  else
    finish(c, -EIO, USBWINE_ERROR_GEN_FAILURE);

  TRACE(4, "ep 0x%02x: request finished, status %d, %d bytes", c->ep,
        status, length);

  if (c->orphaned) {
    unlink_request(h, c);
    free(c);
  }
}

/*
 * Reap every URB that has finished on the device and file each with its
 * request. Never blocks.
 */
static void drain_locked(struct linux_handle *h)
{
  int filed = 0;

  if (h->closing)
    return;

  for (;;) {
    struct usb_urb *urb = NULL;
    struct usb_async *c;

    if (ioctl(h->fd, IOCTL_USB_REAPURBNDELAY, &urb) < 0) {
      if (errno == ENODEV)
        h->gone = 1;		/* unplugged, and nothing left to reap */
      break;
    }
    for (c = h->requests; c && &c->urb != urb; c = c->next)
      ;
    if (!c) {
      TRACE(1, "reaped a URB that no request owns");
      continue;
    }
    complete_locked(h, c);
    filed = 1;
  }

  if (filed)
    wake_locked(h);
}

/* Have the kernel stop c; it lands among the finished URBs right away. */
static void discard_locked(struct linux_handle *h, struct usb_async *c)
{
  /* EINVAL: it had already finished, ENODEV: the device is gone */
  if (ioctl(h->fd, IOCTL_USB_DISCARDURB, &c->urb) < 0 &&
      errno != EINVAL && errno != ENODEV)
    TRACE(1, "ep 0x%02x: could not discard a request: %s", c->ep,
          strerror(errno));
}

/* Stop every request pending on the endpoint, and collect them. */
static void abort_endpoint_locked(struct linux_handle *h, unsigned char ep)
{
  struct usb_async *c;

  for (c = h->requests; c; c = c->next)
    if (c->state == ASYNC_PENDING && c->ep == ep)
      discard_locked(h, c);
  drain_locked(h);
}

/*
 * Sleep on the condition variable until woken or the deadline passes
 * (NULL: no deadline), and for at most nap_ms if that is above 0.
 */
static void sleep_locked(struct linux_handle *h,
	const struct timespec *deadline, int nap_ms)
{
  struct timespec nap;

  if (nap_ms > 0) {
    deadline_in(&nap, nap_ms);
    if (!deadline || deadline->tv_sec > nap.tv_sec ||
        (deadline->tv_sec == nap.tv_sec && deadline->tv_nsec > nap.tv_nsec))
      deadline = &nap;
  }
  if (deadline)
    pthread_cond_timedwait(&h->cond, &h->lock, deadline);
  else
    pthread_cond_wait(&h->cond, &h->lock);
}

/*
 * Sleep in poll() until a URB finishes, wake_locked() is called or ms
 * milliseconds pass (-1: no limit), with the lock dropped meanwhile.
 */
static void poll_locked(struct linux_handle *h, int ms)
{
  struct pollfd fds[2];
  uint64_t count;
  int n;

  h->polling = 1;
  pthread_mutex_unlock(&h->lock);

  fds[0].fd = h->fd;		/* usbfs: writable = a finished URB to reap */
  fds[0].events = POLLOUT;
  fds[0].revents = 0;
  fds[1].fd = h->wake_fd;
  fds[1].events = POLLIN;
  fds[1].revents = 0;
  n = poll(fds, 2, ms);
  if (n > 0 && (fds[1].revents & POLLIN) &&
      read(h->wake_fd, &count, sizeof(count)) < 0) {
    /* nothing to clear */
  }

  pthread_mutex_lock(&h->lock);
  h->polling = 0;
  /* usbfs: hung up = unplugged, and poll() no longer waits */
  if (n > 0 && (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)))
    h->gone = 1;
  pthread_cond_broadcast(&h->cond);	/* another waiter may take over */
}

/*
 * Wait until c has finished, the deadline has passed (NULL: no deadline)
 * or the device is being closed. Returns nonzero if c finished. Called,
 * and returns, with the lock held.
 *
 * One waiting thread at a time sleeps in poll() on the device. When it
 * wakes it files every finished URB with its request; the others sleep on
 * the condition variable, and one of them takes over poll() when it
 * leaves. A thread that files someone else's result, or closes the device,
 * wakes the one in poll() through the eventfd. Nobody sleeps holding the
 * lock, and no completion can go to the wrong thread.
 */
static int wait_for(struct linux_handle *h, struct usb_async *c,
	const struct timespec *deadline)
{
  h->waiters++;

  for (;;) {
    int ms;

    drain_locked(h);
    if (c->state == ASYNC_DONE || h->closing)
      break;
    ms = deadline ? ms_left(deadline) : -1;
    if (ms == 0)
      break;

    if (c->state == ASYNC_IDLE)
      /* never submitted, so nothing will finish it: a Windows wait on its
         event just runs out the clock, and so does this one */
      sleep_locked(h, deadline, 0);
    else if (h->gone)
      /* unplugged: the kernel gives everything back within milliseconds,
         but poll() no longer waits, so nap instead */
      sleep_locked(h, deadline, 10);
    else if (h->polling)
      /* look for itself now and then, in case the thread in poll() is
         held up (suspended, or killed with TerminateThread) */
      sleep_locked(h, deadline, 250);
    else
      poll_locked(h, ms);
  }

  h->waiters--;
  if (h->closing)
    pthread_cond_broadcast(&h->cond);	/* async_close() waits for this */
  return c->state == ASYNC_DONE;
}

static int async_open(usb_dev_handle *dev)
{
  struct linux_handle *h;
  pthread_condattr_t attr;
  int err;

  h = calloc(1, sizeof(*h));
  if (!h)
    USB_ERROR(-ENOMEM);
  h->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (h->wake_fd < 0) {
    err = errno;
    free(h);
    USB_ERROR_STR(win_error(err), "could not open %s/%s: eventfd: %s",
      dev->bus->dirname, dev->device->filename, strerror(err));
  }
  h->fd = dev->fd;
  pthread_mutex_init(&h->lock, NULL);
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&h->cond, &attr);
  pthread_condattr_destroy(&attr);

  dev->impl_info = h;
  return 0;
}

/*
 * End whatever is still pending, the way closing the file handle does on
 * Windows: as aborted. Threads waiting in usb_reap_async() return. The
 * program still owns its contexts and has to free them, but they no
 * longer refer to the device.
 *
 * Returns -1 if a waiting thread did not leave within two seconds (it is
 * suspended, say). Its lock, its eventfd and the device's file stay open
 * for it then, rather than being pulled out from under it.
 */
static int async_close(usb_dev_handle *dev)
{
  struct linux_handle *h = handle_of(dev);
  struct usb_async *c, *next;
  struct timespec deadline;
  int stuck;

  if (!h)
    return 0;
  dev->impl_info = NULL;

  pthread_mutex_lock(&h->lock);
  for (c = h->requests; c; c = c->next)
    if (c->state == ASYNC_PENDING)
      discard_locked(h, c);
  drain_locked(h);
  h->closing = 1;

  for (c = h->requests; c; c = next) {
    next = c->next;
    c->next = NULL;
    if (c->orphaned) {
      /* nobody reaps from now on, so the kernel never writes to it again */
      free(c);
      continue;
    }
    if (c->state == ASYNC_PENDING)
      finish(c, -ETRANSFER_TIMEDOUT, USBWINE_ERROR_OPERATION_ABORTED);
    c->dev = NULL;
  }
  h->requests = NULL;
  wake_locked(h);

  deadline_in(&deadline, 2000);
  while (h->waiters > 0 &&
         pthread_cond_timedwait(&h->cond, &h->lock, &deadline) != ETIMEDOUT)
    ;
  stuck = h->waiters > 0;
  pthread_mutex_unlock(&h->lock);

  if (stuck) {
    TRACE(1, "%s/%s: closed with a thread still waiting; its state is kept for it",
          dev->bus->dirname, dev->device->filename);
    return -1;
  }

  pthread_cond_destroy(&h->cond);
  pthread_mutex_destroy(&h->lock);
  close(h->wake_fd);
  free(h);
  return 0;
}

/*
 * libusb-win32 handles interrupt endpoints just like bulk ones. So does
 * this: the kernel turns a bulk URB on an interrupt endpoint into an
 * interrupt transfer, whereas it refuses an interrupt URB on a bulk one.
 */
static int setup_async(usb_dev_handle *dev, void **context, unsigned char ep,
	const char *what)
{
  struct linux_handle *h = handle_of(dev);
  struct usb_async *c;

  if (!context)
    USB_ERROR_STR(-EINVAL, "%s: invalid context", what);
  *context = NULL;
  if (!h)
    USB_ERROR_STR(-EINVAL, "%s: invalid device handle", what);

  c = calloc(1, sizeof(*c));
  if (!c)
    USB_ERROR_STR(-ENOMEM, "%s: out of memory", what);
  c->dev = dev;
  c->ep = ep;

  pthread_mutex_lock(&h->lock);
  c->next = h->requests;
  h->requests = c;
  pthread_mutex_unlock(&h->lock);

  *context = c;
  return 0;
}

LIBUSB_API int usb_bulk_setup_async(usb_dev_handle *dev, void **context,
	unsigned char ep)
{
  return setup_async(dev, context, ep, "usb_bulk_setup_async");
}

LIBUSB_API int usb_interrupt_setup_async(usb_dev_handle *dev, void **context,
	unsigned char ep)
{
  return setup_async(dev, context, ep, "usb_interrupt_setup_async");
}

LIBUSB_API int usb_submit_async(void *context, char *bytes, int size)
{
  struct usb_async *c = context;
  struct linux_handle *h;
  usb_dev_handle *dev;
  int err;

  if (!c)
    USB_ERROR_STR(-EINVAL, "usb_submit_async: invalid context");
  dev = c->dev;
  h = handle_of(dev);
  if (!h)
    USB_ERROR_STR(-EINVAL, "usb_submit_async: device not open");

  /* the checks libusb-win32 makes */
  if (dev->config <= 0)
    USB_ERROR_STR(-EINVAL, "usb_submit_async: invalid configuration %d",
      dev->config);
  if (dev->interface < 0)
    USB_ERROR_STR(-EINVAL, "usb_submit_async: invalid interface %d",
      dev->interface);

  pthread_mutex_lock(&h->lock);
  if (c->state == ASYNC_PENDING) {
    pthread_mutex_unlock(&h->lock);
    USB_ERROR_STR(-EINVAL, "usb_submit_async: a request is already pending");
  }

  /* like ResetEvent(): until this one finishes, reaps wait */
  c->state = ASYNC_IDLE;
  memset(&c->urb, 0, sizeof(c->urb));
  c->urb.type = USB_URB_TYPE_BULK;	/* see setup_async() */
  c->urb.endpoint = c->ep;
  c->urb.buffer = bytes;
  c->urb.buffer_length = size;

  if (ioctl(h->fd, IOCTL_USB_SUBMITURB, &c->urb) < 0) {
    err = errno;
    if (err == ENODEV)
      h->gone = 1;
    pthread_mutex_unlock(&h->lock);
    USB_ERROR_STR(win_error(err), "usb_submit_async: ep 0x%02x: %s", c->ep,
      strerror(err));
  }
  c->state = ASYNC_PENDING;
  pthread_mutex_unlock(&h->lock);

  TRACE(4, "ep 0x%02x: request submitted, %d bytes", c->ep, size);
  return 0;
}

static int reap(void *context, int timeout, int cancel, const char *what)
{
  struct usb_async *c = context;
  struct linux_handle *h;
  struct timespec deadline;
  unsigned int last_error;
  int result, finished;

  if (!c)
    USB_ERROR_STR(-EINVAL, "%s: invalid context", what);

  h = handle_of(c->dev);
  if (h) {
    if (timeout >= 0)
      deadline_in(&deadline, timeout);

    pthread_mutex_lock(&h->lock);
    finished = wait_for(h, c, timeout >= 0 ? &deadline : NULL);
    if (!finished && cancel && !h->closing && c->state == ASYNC_PENDING) {
      abort_endpoint_locked(h, c->ep);
      /* it may have finished, with data, just before the abort */
      finished = c->state == ASYNC_DONE && c->result >= 0;
    }
    if (finished) {
      result = c->result;
      last_error = c->last_error;
    } else {
      result = -ETRANSFER_TIMEDOUT;
      last_error = h->closing ? USBWINE_ERROR_OPERATION_ABORTED
                              : USBWINE_ERROR_SEM_TIMEOUT;
    }
    pthread_mutex_unlock(&h->lock);
  } else if (c->state == ASYNC_DONE) {
    /* the device is closed; what finished before is still reported */
    result = c->result;
    last_error = c->last_error;
  } else
    USB_ERROR_STR(-EINVAL, "%s: device not open", what);

  if (result < 0) {
    usbwine_set_last_error(last_error);
    USB_ERROR_SET("%s: ep 0x%02x: %s", what, c->ep,
      last_error == USBWINE_ERROR_SEM_TIMEOUT ? "timeout" :
      last_error == USBWINE_ERROR_OPERATION_ABORTED ? "request aborted" :
      "request failed");
  }
  return result;
}

LIBUSB_API int usb_reap_async(void *context, int timeout)
{
  return reap(context, timeout, 1, "usb_reap_async");
}

LIBUSB_API int usb_reap_async_nocancel(void *context, int timeout)
{
  return reap(context, timeout, 0, "usb_reap_async_nocancel");
}

LIBUSB_API int usb_cancel_async(void *context)
{
  struct usb_async *c = context;
  struct linux_handle *h;

  if (!c)
    USB_ERROR_STR(-EINVAL, "usb_cancel_async: invalid context");
  h = handle_of(c->dev);
  if (!h)
    USB_ERROR_STR(-EINVAL, "usb_cancel_async: device not open");

  pthread_mutex_lock(&h->lock);
  abort_endpoint_locked(h, c->ep);
  pthread_mutex_unlock(&h->lock);
  return 0;
}

LIBUSB_API int usb_free_async(void **context)
{
  struct usb_async *c;
  struct linux_handle *h;

  if (!context || !*context)
    USB_ERROR_STR(-EINVAL, "usb_free_async: invalid context");
  c = *context;
  *context = NULL;

  h = handle_of(c->dev);
  if (h) {
    pthread_mutex_lock(&h->lock);
    if (c->state == ASYNC_PENDING) {
      /* libusb-win32 frees it with the request still running, and the
         driver writes to the freed memory later. Stop it first. */
      discard_locked(h, c);
      drain_locked(h);
    }
    if (c->state == ASYNC_PENDING) {
      /* the kernel still has it (the device is going away): free it when
         it comes back, or when the device is closed */
      c->orphaned = 1;
      pthread_mutex_unlock(&h->lock);
      return 0;
    }
    unlink_request(h, c);
    pthread_mutex_unlock(&h->lock);
  }

  free(c);
  return 0;
}
