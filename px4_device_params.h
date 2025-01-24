// px4_device_params.h

#ifndef __PX4_DEVICE_PARAMS_H__
#define __PX4_DEVICE_PARAMS_H__

#include <linux/types.h>

#include "px4_mldev.h"

struct px4_device_param_set {
	unsigned int tsdev_max_packets;
	int psb_purge_timeout;
	bool disable_multi_device_power_control;
	enum px4_mldev_mode multi_device_power_control_mode;
	bool s_tuner_no_sleep;
	bool discard_null_packets;
};

extern struct px4_device_param_set px4_device_params;

#endif
