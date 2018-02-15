/*
 * Some stuff copied and adapted from libusb-win32
 * Stanson <me@stanson.ch>
 */

#ifndef __USB_WINE_H__
#define __USB_WINE_H__

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

#endif /* __USB_WINE_H__ */
