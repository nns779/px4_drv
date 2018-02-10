// it930x-bus.c

// IT930x bus functions

#include "print_format.h"

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "it930x-config.h"
#include "it930x-bus.h"

struct context {
	struct it930x_bus *bus;
	it930x_bus_on_stream_t on_stream;
	void *ctx;
};

struct priv_data {
	struct urb **urbs;
	struct context ctx;
	atomic_t start;
};

static int it930x_usb_ctrl_tx(struct it930x_bus *bus, const void *buf, int len, void *opt)
{
	int ret = 0, rlen = 0;
	struct usb_device *dev = bus->usb.dev;
#if 0
	int sent;
	const u8 *p = buf;
#endif

	if (len > IT930X_USB_MAX_CONTROL_TRANSFER_SIZE || !buf || !len)
		return -EINVAL;

#if 0
	while (len > 0) {
		int s = (len < IT930X_USB_MAX_CONTROL_PACKET_SIZE) ? len : IT930X_USB_MAX_CONTROL_PACKET_SIZE;

		ret = usb_bulk_msg(dev, usb_sndbulkpipe(dev, 0x02), p, s, &rlen, bus->usb.timeout);
		if (ret)
			break;

		p += rlen;
		len -= rlen;
	}
#else
	/* Endpoint 0x02: Control IN */
	ret = usb_bulk_msg(dev, usb_sndbulkpipe(dev, 0x02), (void *)buf, len, &rlen, bus->usb.timeout);
#endif

	if (ret)
		pr_debug("it930x_usb_ctrl_tx: Failed. (ret: %d)\n", ret);

	msleep(1);

	return ret;
}

static int it930x_usb_ctrl_rx(struct it930x_bus *bus, void *buf, int *len, void *opt)
{
	int ret = 0, rlen;
	struct usb_device *dev = bus->usb.dev;

	if (!buf || !len || !*len)
		return -EINVAL;

	/* Endpoint 0x81: Control OUT */
	ret = usb_bulk_msg(dev, usb_rcvbulkpipe(dev, 0x81), buf, *len, &rlen, bus->usb.timeout);
	if (ret)
		pr_debug("it930x_usb_ctrl_rx: Failed. (ret: %d)\n", ret);
	else
		*len = rlen;

	msleep(1);

	return ret;
}

static int it930x_usb_stream_rx(struct it930x_bus *bus, void *buf, int *len)
{
	int ret = 0, rlen;
	struct usb_device *dev = bus->usb.dev;

	if (!buf | !len)
		return -EINVAL;

	/* Endpoint 0x84: Stream OUT */
	ret = usb_bulk_msg(dev, usb_rcvbulkpipe(dev, 0x84), buf, (*len) ? *len : bus->usb.stream_xfer_size, &rlen, bus->usb.timeout);
	if (ret)
		pr_debug("it930x_usb_stream_rx: Failed. (ret: %d)\n", ret);
	else
		*len = rlen;

	return ret;
}

static void free_urbs(struct usb_device *dev, struct urb **urbs, u32 n, bool b)
{
	u32 i;

	if (!urbs)
		return;

	for (i = 0; i < n; i++) {
		struct urb *urb = urbs[i];
		if (urb != NULL) {
			if (urb->transfer_buffer) {
				usb_free_coherent(dev, urb->transfer_buffer_length, urb->transfer_buffer, urb->transfer_dma);
				urb->transfer_buffer = NULL;
				urb->transfer_buffer_length = 0;
			}

			if (b) {
				usb_free_urb(urb);
				urbs[i] = NULL;
			}
		}
	}
}

static void it930x_usb_complete(struct urb *urb)
{
	struct context *ctx = urb->context;

	if (!urb->status) {
		int ret = 0;

		if (urb->actual_length)
			ret = ctx->on_stream(ctx->ctx, urb->transfer_buffer, urb->actual_length);

		if (!ret) {
			ret = usb_submit_urb(urb, GFP_ATOMIC);
			if (ret)
				pr_debug("it930x_usb_complete: usb_submit_urb() failed. (ret: %d)\n", ret);
		}

	} else if (urb->status != -ENOENT) {
		pr_debug("it930x_usb_complete: status: %d\n", urb->status);
	}
}

static int it930x_usb_start_streaming(struct it930x_bus *bus, it930x_bus_on_stream_t on_stream, void *context)
{
	int ret = 0;
	u32 i, n, l;
	struct usb_device *dev = bus->usb.dev;
	struct priv_data *priv = bus->usb.priv;
	struct urb **urbs = priv->urbs;
	struct context *ctx = &priv->ctx;

	if (!on_stream)
		return -EINVAL;

	pr_debug("it930x_usb_start_streaming\n");

	if (atomic_read(&priv->start))
		return 0;

	atomic_set(&priv->start, 1);

	n = bus->usb.stream_urb_num;
	l = bus->usb.stream_xfer_size;

	ctx->on_stream = on_stream;
	ctx->ctx = context;

	for (i = 0; i < n; i++) {
		void *p;
		dma_addr_t dma;

		p = usb_alloc_coherent(dev, l, GFP_ATOMIC, &dma);
		if (!p) {
			free_urbs(dev, urbs, n, false);
			ret = -ENOMEM;
			break;
		}

		usb_fill_bulk_urb(urbs[i], dev, usb_rcvbulkpipe(dev, 0x84), p, l, it930x_usb_complete, ctx);

		urbs[i]->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		urbs[i]->transfer_dma = dma;
	}

	for (i = 0; i < n; i++) {
		ret = usb_submit_urb(urbs[i], GFP_ATOMIC);
		if (ret) {
			int j;

			pr_debug("it930x_usb_start_streaming: usb_submit_urb() failed. (i: %u, ret: %d)\n", i, ret);

			for (j = 0; j < i; j++)
				usb_kill_urb(urbs[j]);

			free_urbs(dev, urbs, n, false);
			break;
		}
	}

	if (ret)
		atomic_set(&priv->start, 0);

	return ret;
}

static int it930x_usb_stop_streaming(struct it930x_bus *bus)
{
	u32 i, n;
	struct usb_device *dev = bus->usb.dev;
	struct priv_data *priv = bus->usb.priv;
	struct urb **urbs = priv->urbs;

	pr_debug("it930x_usb_stop_streaming\n");

	if (!atomic_read(&priv->start))
		return 0;

	n = bus->usb.stream_urb_num;

	for (i = 0; i < n; i++)
		usb_kill_urb(urbs[i]);

	free_urbs(dev, urbs, n, false);

	atomic_set(&priv->start, 0);

	return 0;
}

int it930x_bus_init(struct it930x_bus *bus)
{
	int ret = 0;

	if (!bus)
		return -EINVAL;

	switch(bus->type) {
	case IT930X_BUS_USB:
		if (!bus->usb.dev) {
			ret = -EINVAL;
		} else {
			u32 i, num;
			struct priv_data *priv;
			struct urb **urbs;

			bus->ops.ctrl_tx = it930x_usb_ctrl_tx;
			bus->ops.ctrl_rx = it930x_usb_ctrl_rx;
			bus->ops.stream_rx = it930x_usb_stream_rx;
			bus->ops.start_streaming = it930x_usb_start_streaming;
			bus->ops.stop_streaming = it930x_usb_stop_streaming;

			num = bus->usb.stream_urb_num;

			priv = kzalloc(sizeof(*priv), GFP_ATOMIC);
			if (!priv) {
				ret = -ENOMEM;
				break;
			}

			priv->ctx.bus = bus;
			atomic_set(&priv->start, 0);

			urbs = kcalloc(num, sizeof(*urbs), GFP_KERNEL);
			if (!urbs) {
				kfree(priv);
				ret = -ENOMEM;
				break;
			}

			for (i = 0; i < num; i++) {
				urbs[i] = usb_alloc_urb(0, GFP_ATOMIC | __GFP_ZERO);
				if (!urbs[i]) {
					free_urbs(bus->usb.dev, urbs, num, true);
					kfree(urbs);
					kfree(priv);
					ret = -ENOMEM;
					break;
				}
			}

			usb_get_dev(bus->usb.dev);
			priv->urbs = urbs;
			bus->usb.priv = priv;
		}
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

int it930x_bus_term(struct it930x_bus *bus)
{
	int ret = 0;

	if (!bus) {
		ret = -EINVAL;
		goto exit;
	}

	switch(bus->type) {
	case IT930X_BUS_USB:
	{
		struct priv_data *priv = bus->usb.priv;

		if (priv) {
			if (priv->urbs) {
				free_urbs(bus->usb.dev, priv->urbs, bus->usb.stream_urb_num, true);
				kfree(priv->urbs);
			}
			kfree(priv);
		}
		if (bus->usb.dev)
			usb_put_dev(bus->usb.dev);

		break;
	}

	default:
		break;
	}

	memset(bus, 0, sizeof(struct it930x_bus));

exit:
	return ret;
}

int it930x_bus_ctrl_tx(struct it930x_bus *bus, const void *buf, int len, void *opt)
{
	if (!bus || !bus->ops.ctrl_tx)
		return -EINVAL;

	return bus->ops.ctrl_tx(bus, buf, len, opt);
}

int it930x_bus_ctrl_rx(struct it930x_bus *bus, void *buf, int *len, void *opt)
{
	if (!bus || !bus->ops.ctrl_rx)
		return -EINVAL;

	return bus->ops.ctrl_rx(bus, buf, len, opt);
}

int it930x_bus_stream_rx(struct it930x_bus *bus, void *buf, int *len)
{
	if (!bus || !bus->ops.stream_rx)
		return -EINVAL;

	return bus->ops.stream_rx(bus, buf, len);
}

int it930x_bus_start_streaming(struct it930x_bus *bus, it930x_bus_on_stream_t on_stream, void *context)
{
	if (!bus || !bus->ops.start_streaming)
		return -EINVAL;

	return bus->ops.start_streaming(bus, on_stream, context);
}

int it930x_bus_stop_streaming(struct it930x_bus *bus)
{
	if (!bus || !bus->ops.stop_streaming)
		return -EINVAL;

	return bus->ops.stop_streaming(bus);
}
