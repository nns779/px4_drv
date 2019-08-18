// SPDX-License-Identifier: GPL-2.0-only
/*
 * ITE IT930x bus driver (it930x-bus.c)
 *
 * Copyright (c) 2018-2019 nns779
 */

#include "print_format.h"

#include <linux/version.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/usb.h>

#include "it930x-bus.h"

struct it930x_usb_context;

struct it930x_usb_work {
	struct it930x_usb_context *ctx;
	struct urb *urb;
#ifdef IT930X_BUS_USE_WORKQUEUE
	struct work_struct work;
#endif
};

struct it930x_usb_context {
	struct it930x_bus *bus;
	it930x_bus_on_stream_t on_stream;
	void *ctx;
	u32 num_urb;
	bool no_dma;
#ifdef IT930X_BUS_USE_WORKQUEUE
	struct workqueue_struct *wq;
#endif
	struct it930x_usb_work *works;
	atomic_t start;
};

static int it930x_usb_ctrl_tx(struct it930x_bus *bus, const void *buf, int len, void *opt)
{
	int ret = 0, rlen = 0;
	struct usb_device *dev = bus->usb.dev;
#if 0
	const u8 *p = buf;
#endif

	if (len > 63 || !buf || !len)
		return -EINVAL;

#if 0
	while (len > 0) {
		int s = (len < 255) ? len : 255;

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

	*len = rlen;

	return ret;
}

static void free_urb_buffers(struct usb_device *dev, struct it930x_usb_work *works, u32 n, bool free_urb, bool no_dma)
{
	u32 i;

	if (!works)
		return;

	for (i = 0; i < n; i++) {
		struct urb *urb = works[i].urb;

		if (!urb)
			continue;

		if (urb->transfer_buffer) {
			if (!no_dma)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,34)
				usb_free_coherent(dev, urb->transfer_buffer_length, urb->transfer_buffer, urb->transfer_dma);
#else
				usb_buffer_free(dev, urb->transfer_buffer_length, urb->transfer_buffer, urb->transfer_dma);
#endif
			else
				kfree(urb->transfer_buffer);

			urb->transfer_buffer = NULL;
			urb->transfer_buffer_length = 0;
		}

		if (free_urb) {
			usb_free_urb(urb);
			works[i].urb = NULL;
		}
	}

	return;
}

#ifdef IT930X_BUS_USE_WORKQUEUE
static void it930x_usb_workqueue_handler(struct work_struct *work)
{
	struct it930x_usb_work *w = container_of(work, struct it930x_usb_work, work);
	struct it930x_usb_context *ctx = w->ctx;
	int ret = 0;

	ret = usb_submit_urb(w->urb, GFP_KERNEL);
	if (ret)
		dev_dbg(ctx->bus->dev, "it930x_usb_workqueue_handler: usb_submit_urb() failed. (ret: %d)\n", ret);
}
#endif

static void it930x_usb_complete(struct urb *urb)
{
	int ret = 0;
	struct it930x_usb_work *w = urb->context;
	struct it930x_usb_context *ctx = w->ctx;

	if (urb->status) {
		dev_dbg(ctx->bus->dev, "it930x_usb_complete: status: %d\n", urb->status);
		return;
	}

	if (urb->actual_length)
		ret = ctx->on_stream(ctx->ctx, urb->transfer_buffer, urb->actual_length);
	else
		dev_dbg(ctx->bus->dev, "it930x_usb_complete: !urb->actual_length\n");

	if (!ret && (atomic_read(&ctx->start) == 1)) {
#ifdef IT930X_BUS_USE_WORKQUEUE
		ret = queue_work(ctx->wq, &w->work);
		if (ret)
			dev_dbg(ctx->bus->dev, "it930x_usb_complete: queue_work() failed. (ret: %d)\n", ret);
#else
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret)
			dev_dbg(ctx->bus->dev, "it930x_usb_complete: usb_submit_urb() failed. (ret: %d)\n", ret);
#endif
	}

	return;
}

static int it930x_usb_start_streaming(struct it930x_bus *bus, it930x_bus_on_stream_t on_stream, void *context)
{
	int ret = 0;
	u32 i, l, n;
	bool no_dma;
	struct usb_device *dev = bus->usb.dev;
	struct it930x_usb_context *ctx = bus->usb.priv;
	struct it930x_usb_work *works;

	if (!on_stream)
		return -EINVAL;

	dev_dbg(bus->dev, "it930x_usb_start_streaming\n");

	if (atomic_add_return(2, &ctx->start) != 2) {
		atomic_sub(2, &ctx->start);
		return 0;
	}

	l = bus->usb.streaming_urb_buffer_size;
	n = bus->usb.streaming_urb_num;
	no_dma = bus->usb.streaming_no_dma;

	ctx->on_stream = on_stream;
	ctx->ctx = context;

	works = kcalloc(n, sizeof(*works), GFP_KERNEL);
	if (!works) {
		ret = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < n; i++) {
		struct urb *urb;
		void *p;
		dma_addr_t dma;

		urb = usb_alloc_urb(0, GFP_KERNEL | __GFP_ZERO);
		if (!urb) {
			dev_err(bus->dev, "it930x_usb_start_streaming: usb_alloc_urb() failed. (i: %u)\n", i);
			break;
		}

		if (!no_dma)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,34)
			p = usb_alloc_coherent(dev, l, GFP_KERNEL, &dma);
#else
			p = usb_buffer_alloc(dev, l, GFP_KERNEL, &dma);
#endif
		else
			p = kmalloc(l, GFP_KERNEL);

		if (!p) {
			if (!no_dma)
				dev_err(bus->dev, "it930x_usb_start_streaming: usb_alloc_coherent() failed. (i: %u)\n", i);
			else
				dev_err(bus->dev, "it930x_usb_start_streaming: kmalloc() failed. (i: %u)\n", i);

			usb_free_urb(urb);
			break;
		}

		dev_dbg(bus->dev, "it930x_usb_start_streaming: p: %p, l: %u, dma: %pad\n", p, l, &dma);

		usb_fill_bulk_urb(urb, dev, usb_rcvbulkpipe(dev, 0x84), p, l, it930x_usb_complete, &works[i]);

		if (!no_dma) {
			urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
			urb->transfer_dma = dma;
		}

		works[i].ctx = ctx;
		works[i].urb = urb;
#ifdef IT930X_BUS_USE_WORKQUEUE
		INIT_WORK(&works[i].work, it930x_usb_workqueue_handler);
#endif
	}

	n = i;

	if (!n) {
		ret = -ENOMEM;
		goto fail;
	}

#ifdef IT930X_BUS_USE_WORKQUEUE
	ctx->wq = create_singlethread_workqueue("it930x_usb_workqueue");
	if (!ctx->wq)
		goto fail;
#endif

	usb_reset_endpoint(dev, 0x84);

	for (i = 0; i < n; i++) {
		ret = usb_submit_urb(works[i].urb, GFP_KERNEL);
		if (ret) {
			int j;

			dev_err(bus->dev, "it930x_usb_start_streaming: usb_submit_urb() failed. (i: %u, ret: %d)\n", i, ret);

			for (j = 0; j < i; j++)
				usb_kill_urb(works[i].urb);

			break;
		}
	}

	if (ret)
		goto fail;

	dev_dbg(bus->dev, "it930x_usb_start_streaming: n: %u\n", n);

	ctx->num_urb = n;
	ctx->no_dma = no_dma;
	ctx->works = works;

	atomic_sub(1, &ctx->start);

	return ret;

fail:
#ifdef IT930X_BUS_USE_WORKQUEUE
	if (ctx->wq) {
		flush_workqueue(ctx->wq);
		destroy_workqueue(ctx->wq);
	}
#endif

	free_urb_buffers(dev, works, n, true, no_dma);

	if (works)
		kfree(works);

	ctx->on_stream = NULL;
	ctx->ctx = NULL;
	ctx->num_urb = 0;
	ctx->no_dma = false;
#ifdef IT930X_BUS_USE_WORKQUEUE
	ctx->wq = NULL;
#endif
	ctx->works = NULL;

	atomic_sub(2, &ctx->start);

	return ret;
}

static int it930x_usb_stop_streaming(struct it930x_bus *bus)
{
	u32 i, n;
	struct usb_device *dev = bus->usb.dev;
	struct it930x_usb_context *ctx = bus->usb.priv;
	struct it930x_usb_work *works = ctx->works;

	dev_dbg(bus->dev, "it930x_usb_stop_streaming\n");

	if (atomic_sub_return(2, &ctx->start) != -1) {
		atomic_add(2, &ctx->start);
		return 0;
	}

	n = ctx->num_urb;

#ifdef IT930X_BUS_USE_WORKQUEUE
	if (ctx->wq) {
		flush_workqueue(ctx->wq);
		destroy_workqueue(ctx->wq);
	}
#endif

	if (works) {
		for (i = 0; i < n; i++)
			usb_kill_urb(works[i].urb);

		free_urb_buffers(dev, works, n, true, ctx->no_dma);
		kfree(works);
	}

	ctx->on_stream = NULL;
	ctx->ctx = NULL;
	ctx->num_urb = 0;
	ctx->no_dma = false;
#ifdef IT930X_BUS_USE_WORKQUEUE
	ctx->wq = NULL;
#endif
	ctx->works = NULL;

	atomic_add(1, &ctx->start);

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
			struct it930x_usb_context *ctx;

			ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
			if (!ctx) {
				ret = -ENOMEM;
				break;
			}

			usb_get_dev(bus->usb.dev);

			ctx->bus = bus;
			ctx->on_stream = NULL;
			ctx->ctx = NULL;
			ctx->num_urb = 0;
			ctx->no_dma = false;
#ifdef IT930X_BUS_USE_WORKQUEUE
			ctx->wq = NULL;
#endif
			ctx->works = NULL;
			atomic_set(&ctx->start, 0);

			bus->usb.priv = ctx;

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
		struct it930x_usb_context *ctx = bus->usb.priv;

		if (ctx) {
			it930x_usb_stop_streaming(bus);
			kfree(ctx);
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
