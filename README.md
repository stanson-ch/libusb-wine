This is libusb-win32 for Wine.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

Some Windows applications use libusb-win32. This Wine DLL allows to run
such Windows applications under Wine using native Linux USB stack.

Prerequisites: Wine with winegcc and winebuild

Building and installing:

    $ cd src
    $ make
    $ make install

I didn't port following libusb-win32 functions to libusb-wine:

 * usb_install_service_np
 * usb_uninstall_service_np
 * usb_install_driver_np
 * usb_isochronous_setup_async
 * usb_interrupt_setup_async

however, all Windows applications I tested with libusb-wine do not use them.
