// px4_usb_params.h

#ifndef __PX4_USB_PARAMS_H__
#define __PX4_USB_PARAMS_H__

#include <linux/types.h>

struct px4_usb_param_set {
	int ctrl_timeout;
	unsigned int xfer_packets;
	unsigned int urb_max_packets;
	unsigned int max_urbs;
	bool no_dma;
};

extern struct px4_usb_param_set px4_usb_params;

#endif
