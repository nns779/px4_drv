// SPDX-License-Identifier: GPL-2.0-only
/*
 * ITE IT930x bus driver definitions (it930x-bus.h)
 *
 * Copyright (c) 2018-2019 nns779
 */

#ifndef	__IT930X_BUS_H__
#define __IT930X_BUS_H__

#include <linux/device.h>
#include <linux/usb.h>

typedef enum {
	IT930X_BUS_NONE = 0,
	IT930X_BUS_USB,
} it930x_bus_type_t;

typedef int (*it930x_bus_on_stream_t)(void *context, void *buf, u32 len);

struct it930x_bus;

struct it930x_bus_operations {
	int (*ctrl_tx)(struct it930x_bus *bus, const void *buf, int len, void *opt);
	int (*ctrl_rx)(struct it930x_bus *bus, void *buf, int *len, void *opt);
	int (*stream_rx)(struct it930x_bus *bus, void *buf, int *len, int timeout);
	int (*start_streaming)(struct it930x_bus *bus, it930x_bus_on_stream_t on_stream, void *context);
	int (*stop_streaming)(struct it930x_bus *bus);
};

struct it930x_bus {
	struct device *dev;
	it930x_bus_type_t type;
	union {
		struct {
			struct usb_device *dev;
			int ctrl_timeout;
			u32 streaming_urb_buffer_size;
			u32 streaming_urb_num;
			bool streaming_no_dma;
			void *priv;
		} usb;
	};
	struct it930x_bus_operations ops;
};

int it930x_bus_init(struct it930x_bus *bus);
int it930x_bus_term(struct it930x_bus *bus);

static inline int it930x_bus_ctrl_tx(struct it930x_bus *bus, const void *buf, int len, void *opt)
{
	if (!bus || !bus->ops.ctrl_tx)
		return -EINVAL;

	return bus->ops.ctrl_tx(bus, buf, len, opt);
}

static inline int it930x_bus_ctrl_rx(struct it930x_bus *bus, void *buf, int *len, void *opt)
{
	if (!bus || !bus->ops.ctrl_rx)
		return -EINVAL;

	return bus->ops.ctrl_rx(bus, buf, len, opt);
}

static inline int it930x_bus_stream_rx(struct it930x_bus *bus, void *buf, int *len, int timeout)
{
	if (!bus || !bus->ops.stream_rx)
		return -EINVAL;

	return bus->ops.stream_rx(bus, buf, len, timeout);
}

static inline int it930x_bus_start_streaming(struct it930x_bus *bus, it930x_bus_on_stream_t on_stream, void *context)
{
	if (!bus || !bus->ops.start_streaming)
		return -EINVAL;

	return bus->ops.start_streaming(bus, on_stream, context);
}

static inline int it930x_bus_stop_streaming(struct it930x_bus *bus)
{
	if (!bus || !bus->ops.stop_streaming)
		return -EINVAL;

	return bus->ops.stop_streaming(bus);
}

#endif
