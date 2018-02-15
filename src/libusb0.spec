@ cdecl usb_open                       (ptr)
@ cdecl usb_close                      (ptr)
@ cdecl usb_get_string                 (ptr long long str long)
@ cdecl usb_get_string_simple          (ptr long str long)
@ cdecl usb_get_descriptor_by_endpoint (ptr long long long ptr long)
@ cdecl usb_get_descriptor             (ptr long long ptr long)
@ cdecl usb_bulk_write                 (ptr long str long long)
@ cdecl usb_bulk_read                  (ptr long str long long)
@ cdecl usb_interrupt_write            (ptr long str long long)
@ cdecl usb_interrupt_read             (ptr long str long long)
@ cdecl usb_control_msg                (ptr long long long long str long long)
@ cdecl usb_set_configuration          (ptr long)
@ cdecl usb_claim_interface            (ptr long)
@ cdecl usb_release_interface          (ptr long)
@ cdecl usb_set_altinterface           (ptr long)
@ cdecl usb_resetep                    (ptr long)
@ cdecl usb_clear_halt                 (ptr long)
@ cdecl usb_reset                      (ptr)
@ cdecl usb_strerror                   ()
@ cdecl usb_init                       ()
@ cdecl usb_set_debug                  (long)
@ cdecl usb_find_busses                ()
@ cdecl usb_find_devices               ()
@ cdecl usb_device                     (ptr)
@ cdecl usb_get_busses                 ()
@ cdecl usb_get_version                ()
