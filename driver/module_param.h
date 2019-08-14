// module_param.h

#ifndef __MODULE_PARAM_H__
#define __MODULE_PARAM_H__

#include <linux/types.h>

extern unsigned int xfer_packets;
extern unsigned int urb_max_packets;
extern unsigned int max_urbs;
extern unsigned int tsdev_max_packets;
extern int psb_purge_timeout;
extern bool no_dma;
extern bool disable_multi_device_power_control;
extern bool s_tuner_no_sleep;

#endif
