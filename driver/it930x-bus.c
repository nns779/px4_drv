// it930x-bus.c

// IT930x bus functions

#include "print_format.h"

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/usb.h>

#include "it930x-config.h"
#include "it930x-bus.h"

struct context {
	struct it930x_bus *bus;
	it930x_bus_on_stream_t on_stream;
	void *ctx;
};

struct priv_data_usb {
	struct urb **urbs;
	u32 num_urb;
	bool no_dma;
	struct context ctx;
	atomic_t start;
};

static int it930x_usb_ctrl_tx(struct it930x_bus *bus, const void *buf, int len, void *opt)
{
	int ret = 0, rlen = 0;
	struct usb_device *dev = bus->usb.dev;
#if 0
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
	ret = usb_bulk_msg(dev, usb_sndbulkpipe(dev, 0x02), (void *)buf, len, &rlen, bus->usb.ctrl_timeout);
#endif

	if (ret)
		dev_dbg(bus->dev, "it930x_usb_ctrl_tx: Failed. (ret: %d)\n", ret);

	mdelay(1);

	return ret;
}

static int it930x_usb_ctrl_rx(struct it930x_bus *bus, void *buf, int *len, void *opt)
{
	int ret = 0, rlen = 0;
	struct usb_device *dev = bus->usb.dev;

	if (!buf || !len || !*len)
		return -EINVAL;

	/* Endpoint 0x81: Control OUT */
	ret = usb_bulk_msg(dev, usb_rcvbulkpipe(dev, 0x81), buf, *len, &rlen, bus->usb.ctrl_timeout);
	if (ret)
		dev_dbg(bus->dev, "it930x_usb_ctrl_rx: Failed. (ret: %d)\n", ret);

	*len = rlen;

	mdelay(1);

	return ret;
}

static int it930x_usb_stream_rx(struct it930x_bus *bus, void *buf, int *len, int timeout)
{
	int ret = 0, rlen = 0;
	struct usb_device *dev = bus->usb.dev;

	if (!buf | !len || !*len)
		return -EINVAL;

	/* Endpoint 0x84: Stream OUT */
	ret = usb_bulk_msg(dev, usb_rcvbulkpipe(dev, 0x84), buf, *len, &rlen, timeout);
	if (ret)
		dev_dbg(bus->dev, "it930x_usb_stream_rx: Failed. (ret: %d)\n", ret);

	*len = rlen;

	return ret;
}

static void free_urb_buffers(struct usb_device *dev, struct urb **urbs, u32 n, bool free_urb, bool no_dma)
{
	u32 i;

	if (!urbs)
		return;

	for (i = 0; i < n; i++) {
		struct urb *urb = urbs[i];
		if (urb != NULL) {
			if (urb->transfer_buffer) {
				if (!no_dma)
					usb_free_coherent(dev, urb->transfer_buffer_length, urb->transfer_buffer, urb->transfer_dma);
				else
					kfree(urb->transfer_buffer);

				urb->transfer_buffer = NULL;
				urb->transfer_buffer_length = 0;
			}

			if (free_urb) {
				usb_free_urb(urb);
				urbs[i] = NULL;
			}
		}
	}

	return;
}

static void it930x_usb_complete(struct urb *urb)
{
	int ret = 0;
	struct context *ctx = urb->context;

	if (urb->status) {
		dev_dbg(ctx->bus->dev, "it930x_usb_complete: status: %d\n", urb->status);
		return;
	}

	if (urb->actual_length)
		ret = ctx->on_stream(ctx->ctx, urb->transfer_buffer, urb->actual_length);
	else
		dev_dbg(ctx->bus->dev, "it930x_usb_complete: !urb->actual_length\n");

	if (!ret) {
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret)
			dev_dbg(ctx->bus->dev, "it930x_usb_complete: usb_submit_urb() failed. (ret: %d)\n", ret);
	}

	return;
}

static int it930x_usb_start_streaming(struct it930x_bus *bus, it930x_bus_on_stream_t on_stream, void *context)
{
	int ret = 0;
	u32 i, l, n;
	bool no_dma;
	struct usb_device *dev = bus->usb.dev;
	struct priv_data_usb *priv = bus->usb.priv;
	struct urb **urbs;
	struct context *ctx = &priv->ctx;

	if (!on_stream)
		return -EINVAL;

	dev_dbg(bus->dev, "it930x_usb_start_streaming\n");

	if (atomic_add_return(2, &priv->start) > 2) {
		atomic_sub(2, &priv->start);
		return 0;
	}

	l = bus->usb.streaming_xfer_size;
	n = bus->usb.streaming_urb_num;
	no_dma = bus->usb.streaming_no_dma;

	ctx->on_stream = on_stream;
	ctx->ctx = context;

	urbs = kcalloc(n, sizeof(*urbs), GFP_KERNEL);
	if (!urbs) {
		ret = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < n; i++) {
		void *p;
		dma_addr_t dma;

		urbs[i] = usb_alloc_urb(0, GFP_ATOMIC | __GFP_ZERO);
		if (!urbs[i]) {
			dev_dbg(bus->dev, "it930x_usb_start_streaming: usb_alloc_urb() failed.\n");
			break;
		}

		if (!no_dma)
			p = usb_alloc_coherent(dev, l, GFP_ATOMIC, &dma);
		else
			p = kmalloc(l, GFP_ATOMIC);

		if (!p) {
			dev_dbg(bus->dev, "it930x_usb_start_streaming: usb_alloc_coherent() failed.\n");

			usb_free_urb(urbs[i]);
			urbs[i] = NULL;
			break;
		}

		dev_dbg(bus->dev, "it930x_usb_start_streaming: p: %p, l: %u, dma: %pad\n", p, l, &dma);

		usb_fill_bulk_urb(urbs[i], dev, usb_rcvbulkpipe(dev, 0x84), p, l, it930x_usb_complete, ctx);

		if (!no_dma) {
			urbs[i]->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
			urbs[i]->transfer_dma = dma;
		}
	}

	n = i;

	if (!n) {
		ret = -ENOMEM;
		goto fail;
	}

	usb_reset_endpoint(dev, 0x84);

	for (i = 0; i < n; i++) {
		ret = usb_submit_urb(urbs[i], GFP_ATOMIC);
		if (ret) {
			int j;

			dev_dbg(bus->dev, "it930x_usb_start_streaming: usb_submit_urb() failed. (i: %u, ret: %d)\n", i, ret);

			for (j = 0; j < i; j++)
				usb_kill_urb(urbs[j]);

			break;
		}
	}

	if (ret)
		goto fail;

	dev_dbg(bus->dev, "it930x_usb_start_streaming: n: %u\n", n);

	priv->urbs = urbs;
	priv->num_urb = n;
	priv->no_dma = no_dma;

	atomic_sub(1, &priv->start);

	return ret;

fail:
	free_urb_buffers(dev, urbs, n, true, no_dma);
	kfree(urbs);

	atomic_sub(2, &priv->start);
	return ret;
}

static int it930x_usb_stop_streaming(struct it930x_bus *bus)
{
	u32 i, n;
	struct usb_device *dev = bus->usb.dev;
	struct priv_data_usb *priv = bus->usb.priv;
	struct urb **urbs = priv->urbs;

	dev_dbg(bus->dev, "it930x_usb_stop_streaming\n");

	if (atomic_read(&priv->start) != 1)
		return 0;

	n = priv->num_urb;

	if (urbs) {
		for (i = 0; i < n; i++)
			usb_kill_urb(urbs[i]);

		free_urb_buffers(dev, urbs, n, true, priv->no_dma);
		kfree(urbs);

		priv->urbs = NULL;
	}

	priv->num_urb = 0;
	priv->no_dma = false;

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
			struct priv_data_usb *priv;

			priv = kzalloc(sizeof(*priv), GFP_ATOMIC);
			if (!priv) {
				ret = -ENOMEM;
				break;
			}

			usb_get_dev(bus->usb.dev);

			priv->urbs = NULL;
			priv->num_urb = 0;
			priv->no_dma = false;
			priv->ctx.bus = bus;
			atomic_set(&priv->start, 0);

			bus->usb.priv = priv;

			bus->ops.ctrl_tx = it930x_usb_ctrl_tx;
			bus->ops.ctrl_rx = it930x_usb_ctrl_rx;
			bus->ops.stream_rx = it930x_usb_stream_rx;
			bus->ops.start_streaming = it930x_usb_start_streaming;
			bus->ops.stop_streaming = it930x_usb_stop_streaming;
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
		struct priv_data_usb *priv = bus->usb.priv;

		if (priv) {
			it930x_usb_stop_streaming(bus);
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
