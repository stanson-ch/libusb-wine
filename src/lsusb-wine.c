/*
 * Test utility for libusb-wine.
 * compile with winegcc -o lsusb-wine lsusb-wine.c -lusb0
 * run as ./lsusb-wine.exe, it should list VID/PIDs of connected USB devices
 */

#include <stdio.h>
#include "usb.h"

int main()
{
    struct usb_bus      *usbbus;
    struct usb_device   *usbdev;

    usb_init();
    if( usb_find_busses() < 0 )
    {
        printf( "Can't find busses\n" );
        return 1;
    }

    if( usb_find_devices() < 0 )
    {
        printf( "Can't find devices\n" );
        return 1;
    }

    usbbus = usb_get_busses();
    for( ; usbbus; usbbus = usbbus->next )
    {
        for( usbdev = usbbus->devices; usbdev; usbdev = usbdev->next )
        {
            printf( "%04X:%04X\n", usbdev->descriptor.idVendor, usbdev->descriptor.idProduct );
        }
    }
    return 0;
}
