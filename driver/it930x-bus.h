// it930x-bus.h

#ifndef	__IT930X_BUS_H__
#define __IT930X_BUS_H__

#include <linux/usb.h>

#include "it930x-config.h"

typedef enum {
	IT930X_BUS_NONE = 0,
	IT930X_BUS_USB,
} it930x_bus_type_t;

typedef int (*it930x_bus_on_stream_t)(void *context, void *buf, u32 len);

struct it930x_bus;

struct it930x_bus_operations {
	int (*ctrl_tx)(struct it930x_bus *bus, const void *buf, int len, void *opt);
	int (*ctrl_rx)(struct it930x_bus *bus, void *buf, int *len, void *opt);
	int (*stream_rx)(struct it930x_bus *bus, void *buf, int *len);
	int (*start_streaming)(struct it930x_bus *bus, it930x_bus_on_stream_t on_stream, void *context);
	int (*stop_streaming)(struct it930x_bus *bus);
};

struct it930x_bus {
	it930x_bus_type_t type;
	union {
		struct {
			struct usb_device *dev;
			int timeout;
			u32 stream_xfer_size;
			u32 stream_urb_num;
			void *priv;
		} usb;
	};
	struct it930x_bus_operations ops;
};

int it930x_bus_init(struct it930x_bus *bus);
int it930x_bus_term(struct it930x_bus *bus);
int it930x_bus_ctrl_tx(struct it930x_bus *bus, const void *buf, int len, void *opt);
int it930x_bus_ctrl_rx(struct it930x_bus *bus, void *buf, int *len, void *opt);
int it930x_bus_stream_rx(struct it930x_bus *bus, void *buf, int *len);
int it930x_bus_start_streaming(struct it930x_bus *bus, it930x_bus_on_stream_t on_stream, void *context);
int it930x_bus_stop_streaming(struct it930x_bus *bus);

#endif
