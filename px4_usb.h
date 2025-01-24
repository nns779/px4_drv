// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver definitions for USB devices (px4_usb.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __PX4_USB_H__
#define __PX4_USB_H__

#define USB_PID_PX_W3U4			0x083f
#define USB_PID_PX_Q3U4			0x084a
#define USB_PID_PX_W3PE4		0x023f
#define USB_PID_PX_Q3PE4		0x024a
#define USB_PID_PX_W3PE5		0x073f
#define USB_PID_PX_Q3PE5		0x074a
#define USB_PID_PX_MLT5U		0x084e
#define USB_PID_PX_MLT5PE		0x024e
#define USB_PID_PX_MLT8PE3		0x0252
#define USB_PID_PX_MLT8PE5		0x0253
#define USB_PID_DIGIBEST_ISDB2056		0x004b
#define USB_PID_DIGIBEST_ISDB2056N		0x084b
#define USB_PID_DIGIBEST_ISDB6014_4TS	0x0254
#define USB_PID_PX_M1UR					0x0854
#define USB_PID_PX_S1UR					0x0855


enum px4_usb_device_type {
	UNKNOWN_USB_DEVICE = 0,
	PX4_USB_DEVICE,
	PXMLT5_USB_DEVICE,
	PXMLT8_USB_DEVICE,
	ISDB2056_USB_DEVICE,
	ISDB6014_4TS_USB_DEVICE,
	PXM1UR_USB_DEVICE,
	PXS1UR_USB_DEVICE,
	//----
	MAX_USB_DEVICE_TYPE,
};

int px4_usb_register(void);
void px4_usb_unregister(void);

#endif
