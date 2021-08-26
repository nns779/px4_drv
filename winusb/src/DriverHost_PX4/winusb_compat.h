// winusb_compat.h

#pragma once

#include <windows.h>
#include <winusb.h>

struct usb_device {
	HANDLE dev;
	WINUSB_INTERFACE_HANDLE winusb;
	USB_DEVICE_DESCRIPTOR descriptor;
	USB_STRING_DESCRIPTOR *serial;
};
