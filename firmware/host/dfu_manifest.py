"""Finish a verified PWMVID app update using the custom DFU interface only."""
from __future__ import annotations

import ctypes as C
import ctypes.util


class DeviceDescriptor(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("bLength", C.c_uint8), ("bDescriptorType", C.c_uint8),
        ("bcdUSB", C.c_uint16), ("bDeviceClass", C.c_uint8),
        ("bDeviceSubClass", C.c_uint8), ("bDeviceProtocol", C.c_uint8),
        ("bMaxPacketSize0", C.c_uint8), ("idVendor", C.c_uint16),
        ("idProduct", C.c_uint16), ("bcdDevice", C.c_uint16),
        ("iManufacturer", C.c_uint8), ("iProduct", C.c_uint8),
        ("iSerialNumber", C.c_uint8), ("bNumConfigurations", C.c_uint8),
    ]


def library():
    candidates = [ctypes.util.find_library("usb-1.0"),
                  "/opt/homebrew/lib/libusb-1.0.dylib",
                  "/usr/local/lib/libusb-1.0.dylib", "libusb-1.0.so.0"]
    for name in candidates:
        if name:
            try:
                return C.CDLL(name)
            except OSError:
                pass
    raise RuntimeError("libusb-1.0 is required for the verified USB leave step")


def finish(serial: str, product: str = "PWMVID G431CB USB Updater") -> None:
    """ABORT upload state, validate identity, then manifest an already verified app."""
    usb = library()
    usb.libusb_init.argtypes = [C.POINTER(C.c_void_p)]
    usb.libusb_init.restype = C.c_int
    usb.libusb_exit.argtypes = [C.c_void_p]
    usb.libusb_open_device_with_vid_pid.argtypes = [C.c_void_p, C.c_uint16, C.c_uint16]
    usb.libusb_open_device_with_vid_pid.restype = C.c_void_p
    usb.libusb_get_device.argtypes = [C.c_void_p]
    usb.libusb_get_device.restype = C.c_void_p
    usb.libusb_get_device_descriptor.argtypes = [C.c_void_p, C.POINTER(DeviceDescriptor)]
    usb.libusb_get_device_descriptor.restype = C.c_int
    usb.libusb_get_string_descriptor_ascii.argtypes = [C.c_void_p, C.c_uint8, C.POINTER(C.c_uint8), C.c_int]
    usb.libusb_get_string_descriptor_ascii.restype = C.c_int
    usb.libusb_claim_interface.argtypes = [C.c_void_p, C.c_int]
    usb.libusb_claim_interface.restype = C.c_int
    usb.libusb_release_interface.argtypes = [C.c_void_p, C.c_int]
    usb.libusb_control_transfer.argtypes = [C.c_void_p, C.c_uint8, C.c_uint8,
                                             C.c_uint16, C.c_uint16,
                                             C.POINTER(C.c_uint8), C.c_uint16, C.c_uint]
    usb.libusb_control_transfer.restype = C.c_int
    usb.libusb_close.argtypes = [C.c_void_p]
    context = C.c_void_p()
    if usb.libusb_init(C.byref(context)):
        raise RuntimeError("Unable to initialize libusb")
    handle = None
    claimed = False
    manifested = False
    try:
        handle = usb.libusb_open_device_with_vid_pid(context, 0x0483, 0xDF11)
        if not handle:
            raise RuntimeError("Custom PWMVID DFU device is no longer present")
        desc = DeviceDescriptor()
        if usb.libusb_get_device_descriptor(usb.libusb_get_device(handle), C.byref(desc)):
            raise RuntimeError("Cannot inspect DFU device descriptor")
        def usb_string(index: int) -> str:
            buf = (C.c_uint8 * 128)()
            n = usb.libusb_get_string_descriptor_ascii(handle, index, buf, len(buf))
            if n < 0:
                raise RuntimeError("Cannot read DFU identity strings")
            return bytes(buf[:n]).decode("ascii")
        if (desc.idVendor, desc.idProduct) != (0x0483, 0xDF11) or \
           not desc.iProduct or not desc.iSerialNumber or \
           usb_string(desc.iProduct) != product or \
           usb_string(desc.iSerialNumber) != serial:
            raise RuntimeError("DFU device identity changed; leave request withheld")
        if usb.libusb_claim_interface(handle, 0):
            raise RuntimeError("Cannot claim custom DFU interface for leave request")
        claimed = True
        status = (C.c_uint8 * 6)()
        def transfer(bm: int, request: int, length: int = 0) -> int:
            rc = usb.libusb_control_transfer(handle, bm, request, 0, 0,
                                              status if length else None, length, 2000)
            if rc != length:
                raise RuntimeError(f"DFU request {request} failed ({rc})")
            return rc
        transfer(0x21, 6)  # ABORT the prior UPLOAD_IDLE state.
        transfer(0xA1, 3, 6)
        if status[0] != 0 or status[4] != 2:
            raise RuntimeError("Custom DFU did not return to IDLE before manifestation")
        transfer(0x21, 1)  # Zero-length DNLOAD; loader validates the app footer.
        transfer(0xA1, 3, 6)
        if status[0] != 0 or status[4] != 7:
            raise RuntimeError("Custom DFU did not acknowledge manifestation")
        manifested = True
    finally:
        if handle:
            if claimed:
                usb.libusb_release_interface(handle, 0)
            usb.libusb_close(handle)
        # On macOS libusb 1.0.30 can wait forever for its hotplug thread when
        # the target disconnects during libusb_exit. The process exits after
        # CDC re-enumeration, so let process teardown release this context.
        if not manifested:
            usb.libusb_exit(context)
