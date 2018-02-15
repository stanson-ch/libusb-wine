/*
 * Some stuff copied and adapted from libusb-win32
 * Stanson <me@stanson.ch>
 */

#include <windef.h>
#include "usb-wine.h"

static struct usb_version _usb_version = {
  { VERSION_MAJOR,
    VERSION_MINOR,
    VERSION_MICRO,
    VERSION_NANO },
  { -1, -1, -1, -1 }
};

const struct usb_version *usb_get_version(void)
{
  return &_usb_version;
}

/* DLL main entry point */
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
  switch(reason)
    {
    case DLL_PROCESS_ATTACH:
      break;
    case DLL_PROCESS_DETACH:
      break;
    case DLL_THREAD_ATTACH:
      break;
    case DLL_THREAD_DETACH:
      break;
    default:
      break;
    }
  return TRUE;
}
