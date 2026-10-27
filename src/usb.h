/*
 * Prototypes, structure definitions and macros.
 *
 * Copyright (c) 2000-2003 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 *
 * This file (and only this file) may alternatively be licensed under the
 * BSD license as well, read LICENSE for details.
 */
#ifndef __USB_H__
#define __USB_H__

#include <stdlib.h>

/*
 * USB spec information
 *
 * This is all stuff grabbed from various USB specs and is pretty much
 * not subject to change
 */

/*
 * PATH_MAX from limits.h can't be used on Windows if the dll and
 * import libraries are build/used by different compilers 
 */

#define LIBUSB_PATH_MAX 512

/*
 * Calling convention of the API.
 *
 * Windows programs call these functions with the Microsoft x64 convention:
 * arguments in rcx, rdx, r8 and r9, and rsi and rdi preserved. This library
 * is built by the Linux compiler, whose x86-64 default is the System V
 * convention (arguments in rdi, rsi, ...), and winebuild puts nothing in
 * between. So every exported function has to say which convention it uses.
 * gcc makes the attribute part of the function's type: a definition that
 * leaves it out conflicts with its prototype here and does not compile.
 *
 * On 32-bit x86 the program and the library both use cdecl.
 */
#ifdef __x86_64__
#define LIBUSB_API __attribute__((ms_abi))
#else
#define LIBUSB_API
#endif


/*
 * Device and/or Interface Class codes
 */
#define USB_CLASS_PER_INTERFACE		0	/* for DeviceClass */
#define USB_CLASS_AUDIO			1
#define USB_CLASS_COMM			2
#define USB_CLASS_HID			3
#define USB_CLASS_PRINTER		7
#define USB_CLASS_PTP			6
#define USB_CLASS_MASS_STORAGE		8
#define USB_CLASS_HUB			9
#define USB_CLASS_DATA			10
#define USB_CLASS_VENDOR_SPEC		0xff

/*
 * Descriptor types
 */
#define USB_DT_DEVICE			0x01
#define USB_DT_CONFIG			0x02
#define USB_DT_STRING			0x03
#define USB_DT_INTERFACE		0x04
#define USB_DT_ENDPOINT			0x05

#define USB_DT_HID			0x21
#define USB_DT_REPORT			0x22
#define USB_DT_PHYSICAL			0x23
#define USB_DT_HUB			0x29

/*
 * Descriptor sizes per descriptor type
 */
#define USB_DT_DEVICE_SIZE		18
#define USB_DT_CONFIG_SIZE		9
#define USB_DT_INTERFACE_SIZE		9
#define USB_DT_ENDPOINT_SIZE		7
#define USB_DT_ENDPOINT_AUDIO_SIZE	9	/* Audio extension */
#define USB_DT_HUB_NONVAR_SIZE		7

/* ensure byte-packed structures like libusb-win32 ( Stanson <me@stanson.ch> ) */
#pragma pack(push,1)

/* All standard descriptors have these 2 fields in common */
struct usb_descriptor_header {
	u_int8_t  bLength;
	u_int8_t  bDescriptorType;
};

/* String descriptor */
struct usb_string_descriptor {
	u_int8_t  bLength;
	u_int8_t  bDescriptorType;
	u_int16_t wData[1];
};

/* HID descriptor */
struct usb_hid_descriptor {
	u_int8_t  bLength;
	u_int8_t  bDescriptorType;
	u_int16_t bcdHID;
	u_int8_t  bCountryCode;
	u_int8_t  bNumDescriptors;
	/* u_int8_t  bReportDescriptorType; */
	/* u_int16_t wDescriptorLength; */
	/* ... */
};

/* Endpoint descriptor */
#define USB_MAXENDPOINTS	32
struct usb_endpoint_descriptor {
	u_int8_t  bLength;
	u_int8_t  bDescriptorType;
	u_int8_t  bEndpointAddress;
	u_int8_t  bmAttributes;
	u_int16_t wMaxPacketSize;
	u_int8_t  bInterval;
	u_int8_t  bRefresh;
	u_int8_t  bSynchAddress;

	unsigned char *extra;	/* Extra descriptors */
	int extralen;
};

#define USB_ENDPOINT_ADDRESS_MASK	0x0f    /* in bEndpointAddress */
#define USB_ENDPOINT_DIR_MASK		0x80

#define USB_ENDPOINT_TYPE_MASK		0x03    /* in bmAttributes */
#define USB_ENDPOINT_TYPE_CONTROL	0
#define USB_ENDPOINT_TYPE_ISOCHRONOUS	1
#define USB_ENDPOINT_TYPE_BULK		2
#define USB_ENDPOINT_TYPE_INTERRUPT	3

/* Interface descriptor */
#define USB_MAXINTERFACES	32
struct usb_interface_descriptor {
	u_int8_t  bLength;
	u_int8_t  bDescriptorType;
	u_int8_t  bInterfaceNumber;
	u_int8_t  bAlternateSetting;
	u_int8_t  bNumEndpoints;
	u_int8_t  bInterfaceClass;
	u_int8_t  bInterfaceSubClass;
	u_int8_t  bInterfaceProtocol;
	u_int8_t  iInterface;

	struct usb_endpoint_descriptor *endpoint;

	unsigned char *extra;	/* Extra descriptors */
	int extralen;
};

#define USB_MAXALTSETTING	128	/* Hard limit */
struct usb_interface {
	struct usb_interface_descriptor *altsetting;

	int num_altsetting;
};

/* Configuration descriptor information.. */
#define USB_MAXCONFIG		8
struct usb_config_descriptor {
	u_int8_t  bLength;
	u_int8_t  bDescriptorType;
	u_int16_t wTotalLength;
	u_int8_t  bNumInterfaces;
	u_int8_t  bConfigurationValue;
	u_int8_t  iConfiguration;
	u_int8_t  bmAttributes;
	u_int8_t  MaxPower;

	struct usb_interface *interface;

	unsigned char *extra;	/* Extra descriptors */
	int extralen;
};

/* Device descriptor */
struct usb_device_descriptor {
	u_int8_t  bLength;
	u_int8_t  bDescriptorType;
	u_int16_t bcdUSB;
	u_int8_t  bDeviceClass;
	u_int8_t  bDeviceSubClass;
	u_int8_t  bDeviceProtocol;
	u_int8_t  bMaxPacketSize0;
	u_int16_t idVendor;
	u_int16_t idProduct;
	u_int16_t bcdDevice;
	u_int8_t  iManufacturer;
	u_int8_t  iProduct;
	u_int8_t  iSerialNumber;
	u_int8_t  bNumConfigurations;
};

struct usb_ctrl_setup {
	u_int8_t  bRequestType;
	u_int8_t  bRequest;
	u_int16_t wValue;
	u_int16_t wIndex;
	u_int16_t wLength;
};

/*
 * Standard requests
 */
#define USB_REQ_GET_STATUS		0x00
#define USB_REQ_CLEAR_FEATURE		0x01
/* 0x02 is reserved */
#define USB_REQ_SET_FEATURE		0x03
/* 0x04 is reserved */
#define USB_REQ_SET_ADDRESS		0x05
#define USB_REQ_GET_DESCRIPTOR		0x06
#define USB_REQ_SET_DESCRIPTOR		0x07
#define USB_REQ_GET_CONFIGURATION	0x08
#define USB_REQ_SET_CONFIGURATION	0x09
#define USB_REQ_GET_INTERFACE		0x0A
#define USB_REQ_SET_INTERFACE		0x0B
#define USB_REQ_SYNCH_FRAME		0x0C

#define USB_TYPE_STANDARD		(0x00 << 5)
#define USB_TYPE_CLASS			(0x01 << 5)
#define USB_TYPE_VENDOR			(0x02 << 5)
#define USB_TYPE_RESERVED		(0x03 << 5)

#define USB_RECIP_DEVICE		0x00
#define USB_RECIP_INTERFACE		0x01
#define USB_RECIP_ENDPOINT		0x02
#define USB_RECIP_OTHER			0x03

/*
 * Various libusb API related stuff
 */

#define USB_ENDPOINT_IN			0x80
#define USB_ENDPOINT_OUT		0x00

/* Error codes */
#define USB_ERROR_BEGIN			500000

/*
 * This is supposed to look weird. This file is generated from autoconf
 * and I didn't want to make this too complicated.
 */
#if 0
#define USB_LE16_TO_CPU(x) do { x = ((x & 0xff) << 8) | ((x & 0xff00) >> 8); } while(0)
#else
#define USB_LE16_TO_CPU(x)
#endif

/* Data types */
struct usb_device;
struct usb_bus;

/*
 * To maintain compatibility with applications already built with libusb,
 * we must only add entries to the end of this structure. NEVER delete or
 * move members and only change types if you really know what you're doing.
 */
struct usb_device {
  struct usb_device *next, *prev;

  char filename[LIBUSB_PATH_MAX];

  struct usb_bus *bus;

  struct usb_device_descriptor descriptor;
  struct usb_config_descriptor *config;

  void *dev;		/* Darwin support */

  u_int8_t devnum;

  unsigned char num_children;
  struct usb_device **children;
};

struct usb_bus {
  struct usb_bus *next, *prev;

  char dirname[LIBUSB_PATH_MAX];

  struct usb_device *devices;
  u_int32_t location;

  struct usb_device *root_dev;
};

struct usb_dev_handle;
typedef struct usb_dev_handle usb_dev_handle;

/* Variables */
extern struct usb_bus *usb_busses;

/* release byte-packing ( Stanson <me@stanson.ch> ) */
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif

/* Function prototypes */

/* usb.c */
LIBUSB_API usb_dev_handle *usb_open(struct usb_device *dev);
LIBUSB_API int usb_close(usb_dev_handle *dev);
LIBUSB_API int usb_get_string(usb_dev_handle *dev, int index, int langid,
	char *buf, size_t buflen);
LIBUSB_API int usb_get_string_simple(usb_dev_handle *dev, int index,
	char *buf, size_t buflen);

/* descriptors.c */
LIBUSB_API int usb_get_descriptor_by_endpoint(usb_dev_handle *udev, int ep,
	unsigned char type, unsigned char index, void *buf, int size);
LIBUSB_API int usb_get_descriptor(usb_dev_handle *udev, unsigned char type,
	unsigned char index, void *buf, int size);

/* <arch>.c */
LIBUSB_API int usb_bulk_write(usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout);
LIBUSB_API int usb_bulk_read(usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout);
LIBUSB_API int usb_interrupt_write(usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout);
LIBUSB_API int usb_interrupt_read(usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout);
LIBUSB_API int usb_control_msg(usb_dev_handle *dev, int requesttype,
	int request, int value, int index, char *bytes, int size, int timeout);
LIBUSB_API int usb_set_configuration(usb_dev_handle *dev, int configuration);
LIBUSB_API int usb_claim_interface(usb_dev_handle *dev, int interface);
LIBUSB_API int usb_release_interface(usb_dev_handle *dev, int interface);
LIBUSB_API int usb_set_altinterface(usb_dev_handle *dev, int alternate);
LIBUSB_API int usb_resetep(usb_dev_handle *dev, unsigned int ep);
LIBUSB_API int usb_clear_halt(usb_dev_handle *dev, unsigned int ep);
LIBUSB_API int usb_reset(usb_dev_handle *dev);

#if 1
#define LIBUSB_HAS_GET_DRIVER_NP 1
LIBUSB_API int usb_get_driver_np(usb_dev_handle *dev, int interface,
	char *name, unsigned int namelen);
#define LIBUSB_HAS_DETACH_KERNEL_DRIVER_NP 1
LIBUSB_API int usb_detach_kernel_driver_np(usb_dev_handle *dev, int interface);
#endif

LIBUSB_API const char *usb_strerror(void);

LIBUSB_API void usb_init(void);
LIBUSB_API void usb_set_debug(int level);
LIBUSB_API int usb_find_busses(void);
LIBUSB_API int usb_find_devices(void);
LIBUSB_API struct usb_device *usb_device(usb_dev_handle *dev);
LIBUSB_API struct usb_bus *usb_get_busses(void);


LIBUSB_API int usb_bulk_setup_async(usb_dev_handle *dev, void **context,
	unsigned char ep);
LIBUSB_API int usb_interrupt_setup_async(usb_dev_handle *dev, void **context,
	unsigned char ep);

LIBUSB_API int usb_submit_async(void *context, char *bytes, int size);
LIBUSB_API int usb_reap_async(void *context, int timeout);
LIBUSB_API int usb_reap_async_nocancel(void *context, int timeout);
LIBUSB_API int usb_free_async(void **context);
LIBUSB_API int usb_cancel_async(void *context);

#ifdef __cplusplus
}
#endif

#endif /* __USB_H__ */

