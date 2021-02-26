// SPDX-License-Identifier: GPL-2.0-only
/*
 * ITE IT930x bus driver definitions (itedtv_bus.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __ITEDTV_BUS_H__
#define __ITEDTV_BUS_H__

#ifdef __linux__
#include <linux/device.h>
#include <linux/usb.h>
#elif defined(_WIN32) || defined(_WIN64)
#include "misc_win.h"
#include "winusb_compat.h"
#endif

enum itedtv_bus_type {
	ITEDTV_BUS_NONE = 0,
	ITEDTV_BUS_USB,
};

typedef int (*itedtv_bus_stream_handler_t)(void *context, void *buf, u32 len);

struct itedtv_bus;

struct itedtv_bus_operations {
	int (*ctrl_tx)(struct itedtv_bus *bus, void *buf, int len);
	int (*ctrl_rx)(struct itedtv_bus *bus, void *buf, int *len);
	int (*stream_rx)(struct itedtv_bus *bus,
			 void *buf, int *len,
			 int timeout);
	int (*start_streaming)(struct itedtv_bus *bus,
			       itedtv_bus_stream_handler_t stream_handler,
			       void *context);
	int (*stop_streaming)(struct itedtv_bus *bus);
};

struct itedtv_bus {
	struct device *dev;
	enum itedtv_bus_type type;
	union {
		struct {
			struct usb_device *dev;
			int ctrl_timeout;
			int max_bulk_size;
			struct {
				u32 urb_buffer_size;
				u32 urb_num;
				bool no_dma;	// for Linux
				bool no_raw_io;	// for Windows(WinUSB)
			} streaming;
			void *priv;
		} usb;
	};
	struct itedtv_bus_operations ops;
};

#ifdef __cplusplus
extern "C" {
#endif
int itedtv_bus_init(struct itedtv_bus *bus);
int itedtv_bus_term(struct itedtv_bus *bus);
#ifdef __cplusplus
}
#endif

static inline int itedtv_bus_ctrl_tx(struct itedtv_bus *bus,
				     void *buf, int len)
{
	if (!bus || !bus->ops.ctrl_tx)
		return -EINVAL;

	return bus->ops.ctrl_tx(bus, buf, len);
}

static inline int itedtv_bus_ctrl_rx(struct itedtv_bus *bus,
				     void *buf, int *len)
{
	if (!bus || !bus->ops.ctrl_rx)
		return -EINVAL;

	return bus->ops.ctrl_rx(bus, buf, len);
}

static inline int itedtv_bus_stream_rx(struct itedtv_bus *bus,
				       void *buf, int *len,
				       int timeout)
{
	if (!bus || !bus->ops.stream_rx)
		return -EINVAL;

	return bus->ops.stream_rx(bus, buf, len, timeout);
}

static inline int itedtv_bus_start_streaming(struct itedtv_bus *bus,
					     itedtv_bus_stream_handler_t stream_handler,
					     void *context)
{
	if (!bus || !bus->ops.start_streaming)
		return -EINVAL;

	return bus->ops.start_streaming(bus, stream_handler, context);
}

static inline int itedtv_bus_stop_streaming(struct itedtv_bus *bus)
{
	if (!bus || !bus->ops.stop_streaming)
		return -EINVAL;

	return bus->ops.stop_streaming(bus);
}

#endif
