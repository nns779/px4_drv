// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver definitions for USB devices (px4_usb.h)
 *
 * Copyright (c) 2018-2020 nns779
 */

#ifndef __PX4_USB_H__
#define __PX4_USB_H__

#define USB_PID_PX_W3U4			0x083f
#define USB_PID_PX_Q3U4			0x084a
#define USB_PID_PX_W3PE4		0x023f
#define USB_PID_PX_Q3PE4		0x024a
#define USB_PID_PX_W3PE5		0x073f
#define USB_PID_PX_Q3PE5		0x074a
#define USB_PID_PX_MLT5PE		0x024e
#define USB_PID_PX_MLT8PE3		0x0252
#define USB_PID_PX_MLT8PE5		0x0253
#define USB_PID_DIGIBEST_ISDB2056	0x004b

enum px4_usb_device_type {
	UNKNOWN_USB_DEVICE = 0,
	PX4_USB_DEVICE,
	PXMLT5_USB_DEVICE,
	PXMLT8_USB_DEVICE,
	ISDB2056_USB_DEVICE
};

int px4_usb_register(void);
void px4_usb_unregister(void);

#endif
