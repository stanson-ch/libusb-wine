/*
 * Some stuff copied and adapted from libusb-win32
 * Stanson <me@stanson.ch>
 */

#ifndef __USB_WINE_H__
#define __USB_WINE_H__

#include "usb.h"

#define VERSION_MAJOR 0
#define VERSION_MINOR 1
#define VERSION_MICRO 12
#define VERSION_NANO  1

/* Version information, Windows specific */
struct usb_version {
  struct {
    int major;
    int minor;
    int micro;
    int nano;
  } dll;
  struct {
    int major;
    int minor;
    int micro;
    int nano;
  } driver;
};

LIBUSB_API const struct usb_version *usb_get_version(void);

/*
 * Win32 error codes that libusb-win32 leaves for GetLastError() after a
 * failed reap. Programs look at them: ANT_DLL takes a timed-out reap whose
 * last error is ERROR_OPERATION_ABORTED to mean the stick lost power.
 */
#define USBWINE_ERROR_GEN_FAILURE		31
#define USBWINE_ERROR_SEM_TIMEOUT		121
#define USBWINE_ERROR_OPERATION_ABORTED		995

/* SetLastError() for the calling thread. It lives in usb-wine.c, so that
   the rest of the library needs no Windows headers. */
void usbwine_set_last_error(unsigned int code);

#endif /* __USB_WINE_H__ */
