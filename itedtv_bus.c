// SPDX-License-Identifier: GPL-2.0-only
/*
 * ITE IT930x bus driver (itedtv_bus.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "print_format.h"
#include "itedtv_bus.h"

#ifdef __linux__
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#endif

#if defined(ITEDTV_BUS_USE_WORKQUEUE) && !defined(__linux__)
#undef ITEDTV_BUS_USE_WORKQUEUE
#endif

struct itedtv_usb_context;

struct itedtv_usb_work {
	struct itedtv_usb_context *ctx;
	struct urb *urb;
#ifdef ITEDTV_BUS_USE_WORKQUEUE
	struct work_struct work;
#endif
};

struct itedtv_usb_context {
	struct mutex lock;
	struct itedtv_bus *bus;
	itedtv_bus_stream_handler_t stream_handler;
	void *ctx;
	u32 num_urb;
	bool no_dma;
#ifdef ITEDTV_BUS_USE_WORKQUEUE
	struct workqueue_struct *wq;
#endif
	u32 num_works;
	struct itedtv_usb_work *works;
	atomic_t streaming;
};

static int itedtv_usb_ctrl_tx(struct itedtv_bus *bus, void *buf, int len)
{
	int ret = 0, rlen = 0;
	struct usb_device *dev = bus->usb.dev;

	if (unlikely(!buf || !len))
		return -EINVAL;

	/* Endpoint 0x02: Host->Device bulk endpoint for controlling the device */
	ret = usb_bulk_msg(dev,
			   usb_sndbulkpipe(dev, 0x02),
			   buf, len,
			   &rlen, bus->usb.ctrl_timeout);

	if (ret) {
		dev_err(bus->dev,
			 "itedtv_usb_ctrl_tx: usb_bulk_msg() failed. (ret: %d)\n",
			 ret);
	}

	mdelay(1);

	return ret;
}

static int itedtv_usb_ctrl_rx(struct itedtv_bus *bus, void *buf, int *len)
{
	int ret = 0, rlen = 0;
	struct usb_device *dev = bus->usb.dev;

	if (unlikely(!buf || !len || !*len))
		return -EINVAL;

	/* Endpoint 0x81: Device->Host bulk endpoint for controlling the device */
	ret = usb_bulk_msg(dev,
			   usb_rcvbulkpipe(dev, 0x81),
			   buf, *len,
			   &rlen, bus->usb.ctrl_timeout);

	if (ret) {
		dev_err(bus->dev,
			 "itedtv_usb_ctrl_rx: usb_bulk_msg() failed. (ret: %d)\n",
			 ret);
	}

	*len = rlen;

	mdelay(1);

	return ret;
}

static int itedtv_usb_stream_rx(struct itedtv_bus *bus,
				void *buf, int *len,
				int timeout)
{
	int ret = 0, rlen = 0;
	struct usb_device *dev = bus->usb.dev;

	if (unlikely(!buf | !len || !*len))
		return -EINVAL;

	/* Endpoint 0x84: Device->Host bulk endpoint for receiving TS from the device */
	ret = usb_bulk_msg(dev,
			   usb_rcvbulkpipe(dev, 0x84),
			   buf, *len,
			   &rlen, timeout);

	*len = rlen;

	return ret;
}

#ifdef ITEDTV_BUS_USE_WORKQUEUE
static void itedtv_usb_workqueue_handler(struct work_struct *work)
{
	int ret = 0;
	struct itedtv_usb_work *w = container_of(work,
						 struct itedtv_usb_work, work);
	struct itedtv_usb_context *ctx = w->ctx;
	struct urb *urb = w->urb;

	if (likely(urb->actual_length))
		ret = ctx->stream_handler(ctx->ctx,
					  urb->transfer_buffer,
					  urb->actual_length);
	else
		dev_dbg(ctx->bus->dev,
			"itedtv_usb_workqueue_handler: !urb->actual_length\n");

	if (unlikely(ret || (atomic_read_acquire(&ctx->streaming) < 1)))
		return;

	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (unlikely(ret))
		dev_err(ctx->bus->dev,
			"itedtv_usb_workqueue_handler: usb_submit_urb() failed. (ret: %d)\n",
			ret);

	return;
}
#endif

static void itedtv_usb_complete(struct urb *urb)
{
#ifndef ITEDTV_BUS_USE_WORKQUEUE
	int ret = 0;
#endif
	struct itedtv_usb_work *w = urb->context;
	struct itedtv_usb_context *ctx = w->ctx;

	if (unlikely(urb->status)) {
		dev_dbg(ctx->bus->dev,
			"itedtv_usb_complete: status: %d\n",
			urb->status);
		return;
	}

#ifdef ITEDTV_BUS_USE_WORKQUEUE
	if (unlikely(!queue_work(ctx->wq, &w->work)))
		dev_err(ctx->bus->dev,
			"itedtv_usb_complete: queue_work() failed.\n");
#else
	if (likely(urb->actual_length))
		ret = ctx->stream_handler(ctx->ctx,
					  urb->transfer_buffer,
					  urb->actual_length);
	else
		dev_dbg(ctx->bus->dev,
			"itedtv_usb_complete: !urb->actual_length\n");

	if (unlikely(ret || (atomic_read_acquire(&ctx->streaming) < 1)))
		return;

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (unlikely(ret))
		dev_err(ctx->bus->dev,
			"itedtv_usb_complete: usb_submit_urb() failed. (ret: %d)\n",
			ret);
#endif

	return;
}

static int itedtv_usb_alloc_urb_buffers(struct itedtv_usb_context *ctx,
					u32 buf_size)
{
	u32 i;
	struct itedtv_bus *bus = ctx->bus;
	struct usb_device *dev = bus->usb.dev;
	u32 num = ctx->num_works;
	bool no_dma = ctx->no_dma;
	struct itedtv_usb_work *works = ctx->works;

	if (!works)
		return -EINVAL;

	for (i = 0; i < num; i++) {
		struct urb *urb;
		void *p;
#ifdef __linux__
		dma_addr_t dma;
#endif

		if (works[i].urb) {
			urb = works[i].urb;

			if (urb->transfer_buffer) {
#ifdef __linux__
				if ((urb->transfer_flags & URB_NO_TRANSFER_DMA_MAP) &&
				    (no_dma || urb->transfer_buffer_length != buf_size)) {
					usb_free_coherent(dev,
							  urb->transfer_buffer_length,
							  urb->transfer_buffer,
							  urb->transfer_dma);
					urb->transfer_flags &= ~URB_NO_TRANSFER_DMA_MAP;
					urb->transfer_dma = 0;

					urb->transfer_buffer = NULL;
					urb->transfer_buffer_length = 0;
					urb->actual_length = 0;
				} else if (!(urb->transfer_flags & URB_NO_TRANSFER_DMA_MAP) &&
					   (!no_dma || urb->transfer_buffer_length != buf_size)) {
					kfree(urb->transfer_buffer);

					urb->transfer_buffer = NULL;
					urb->transfer_buffer_length = 0;
					urb->actual_length = 0;
				}
#else
				kfree(urb->transfer_buffer);

				urb->transfer_buffer = NULL;
				urb->transfer_buffer_length = 0;
				urb->actual_length = 0;
#endif
			}
		} else {
			urb = usb_alloc_urb(0, GFP_KERNEL);
			if (!urb) {
				dev_err(bus->dev,
					"itedtv_usb_alloc_urb_buffers: usb_alloc_urb() failed. (i: %u)\n",
					i);
				break;
			}

			works[i].urb = urb;
		}

		works[i].ctx = ctx;

		if (!urb->transfer_buffer) {
#ifdef __linux__
#ifdef __GFP_RETRY_MAYFAIL
			if (!no_dma)
				p = usb_alloc_coherent(dev, buf_size,
						       GFP_KERNEL | __GFP_RETRY_MAYFAIL, &dma);
			else
				p = kmalloc(buf_size, GFP_KERNEL | __GFP_RETRY_MAYFAIL);
#else
			if (!no_dma)
				p = usb_alloc_coherent(dev, buf_size,
						       GFP_KERNEL | __GFP_REPEAT, &dma);
			else
				p = kmalloc(buf_size, GFP_KERNEL | __GFP_REPEAT);
#endif
#else
			p = kmalloc(buf_size, GFP_KERNEL);
#endif

			if (!p) {
#ifdef __linux__
				if (!no_dma)
					dev_err(bus->dev,
						"itedtv_usb_alloc_urb_buffers: usb_alloc_coherent() failed. (i: %u)\n",
						i);
				else
					dev_err(bus->dev,
						"itedtv_usb_alloc_urb_buffers: kmalloc() failed. (i: %u)\n",
						i);
#else
				dev_err(bus->dev,
					"itedtv_usb_alloc_urb_buffers: kmalloc() failed. (i: %u)\n",
					i);
#endif

				usb_free_urb(urb);
				works[i].urb = NULL;

				break;
			}

#ifdef __linux__
			dev_dbg(bus->dev,
				"itedtv_usb_alloc_urb_buffers: p: %p, buf_size: %u, dma: %pad\n",
				p, buf_size, &dma);
#else
			dev_dbg(bus->dev,
				"itedtv_usb_alloc_urb_buffers: p: %p, buf_size: %u\n",
				p, buf_size);
#endif

			usb_fill_bulk_urb(urb, dev,
					  usb_rcvbulkpipe(dev, 0x84),
					  p, buf_size,
					  itedtv_usb_complete, &works[i]);

#ifdef __linux__
			if (!no_dma) {
				urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
				urb->transfer_dma = dma;
			}
#endif
		}

#ifdef ITEDTV_BUS_USE_WORKQUEUE
		INIT_WORK(&works[i].work, itedtv_usb_workqueue_handler);
#endif
	}

	ctx->num_urb = i;

	if (!i)
		return -ENOMEM;

	return 0;
}

static void itedtv_usb_free_urb_buffers(struct itedtv_usb_context *ctx,
					bool free_urb)
{
	u32 i;
	struct usb_device *dev = ctx->bus->usb.dev;
	u32 num = ctx->num_works;
	bool no_dma = ctx->no_dma;
	struct itedtv_usb_work *works = ctx->works;

	if (!works)
		return;

	for (i = 0; i < num; i++) {
		struct urb *urb = works[i].urb;

		if (!urb)
			continue;

		if (urb->transfer_buffer) {
#ifdef __linux__
			if (!no_dma) {
				usb_free_coherent(dev,
						  urb->transfer_buffer_length,
						  urb->transfer_buffer,
						  urb->transfer_dma);
				urb->transfer_flags &= ~URB_NO_TRANSFER_DMA_MAP;
				urb->transfer_dma = 0;
			} else {
				kfree(urb->transfer_buffer);
			}
#else
			kfree(urb->transfer_buffer);
#endif

			urb->transfer_buffer = NULL;
			urb->transfer_buffer_length = 0;
			urb->actual_length = 0;
		}

		if (free_urb) {
			usb_free_urb(urb);
			works[i].urb = NULL;
		}
	}

	if (free_urb)
		ctx->num_urb = 0;

	return;
}

static void itedtv_usb_clean_context(struct itedtv_usb_context *ctx, bool free_works)
{
#ifdef ITEDTV_BUS_USE_WORKQUEUE
	if (ctx->wq)
		destroy_workqueue(ctx->wq);
#endif

	if (free_works && ctx->works) {
		itedtv_usb_free_urb_buffers(ctx, true);
		kfree(ctx->works);
		ctx->num_urb = 0;
		ctx->works = NULL;
		ctx->num_works = 0;
	}

	ctx->stream_handler = NULL;
	ctx->ctx = NULL;
	ctx->no_dma = false;
#ifdef ITEDTV_BUS_USE_WORKQUEUE
	ctx->wq = NULL;
#endif

	return;
}

static int itedtv_usb_start_streaming(struct itedtv_bus *bus,
				      itedtv_bus_stream_handler_t stream_handler,
				      void *context)
{
	int ret = 0;
	u32 i, buf_size, num;
	struct itedtv_usb_context *ctx = bus->usb.priv;
	struct itedtv_usb_work *works;

	if (!stream_handler)
		return -EINVAL;

	dev_dbg(bus->dev, "itedtv_usb_start_streaming\n");

	mutex_lock(&ctx->lock);

	ctx->stream_handler = stream_handler;
	ctx->ctx = context;

	buf_size = bus->usb.streaming.urb_buffer_size;
	num = bus->usb.streaming.urb_num;
	ctx->no_dma = bus->usb.streaming.no_dma;

	if (ctx->works && num != ctx->num_works) {
		itedtv_usb_free_urb_buffers(ctx, true);
		kfree(ctx->works);
		ctx->works = NULL;
	}

	ctx->num_works = num;

	if (!ctx->works) {
		ctx->works = kcalloc(ctx->num_works,
				     sizeof(*works), GFP_KERNEL);
		if (!ctx->works) {
			ret = -ENOMEM;
			goto fail;
		}
	}

	ret = itedtv_usb_alloc_urb_buffers(ctx, buf_size);
	if (ret)
		goto fail;

#ifdef ITEDTV_BUS_USE_WORKQUEUE
	if (!ctx->wq) {
		ctx->wq = create_singlethread_workqueue("itedtv_usb_workqueue");
		if (!ctx->wq) {
			ret = -ENOMEM;
			goto fail;
		}
	}
#endif

	usb_reset_endpoint(bus->usb.dev, 0x84);
	atomic_xchg(&ctx->streaming, 1);

	num = ctx->num_urb;
	works = ctx->works;

	for (i = 0; i < num; i++) {
		ret = usb_submit_urb(works[i].urb, GFP_KERNEL);
		if (ret) {
			u32 j;

			dev_err(bus->dev,
				"itedtv_usb_start_streaming: usb_submit_urb() failed. (i: %u, ret: %d)\n",
				i, ret);

			for (j = 0; j < i; j++)
				usb_kill_urb(works[i].urb);

			break;
		}
	}

	if (ret)
		goto fail;

	dev_dbg(bus->dev, "itedtv_usb_start_streaming: num: %u\n", num);

	mutex_unlock(&ctx->lock);

	return ret;

fail:
	atomic_xchg(&ctx->streaming, 0);

#ifdef ITEDTV_BUS_USE_WORKQUEUE
	if (ctx->wq)
		flush_workqueue(ctx->wq);
#endif

	itedtv_usb_clean_context(ctx, true);

	mutex_unlock(&ctx->lock);

	return ret;
}

static int itedtv_usb_stop_streaming(struct itedtv_bus *bus)
{
	u32 i;
	struct itedtv_usb_context *ctx = bus->usb.priv;

	dev_dbg(bus->dev, "itedtv_usb_stop_streaming\n");

	mutex_lock(&ctx->lock);

	atomic_xchg(&ctx->streaming, 0);

#ifdef ITEDTV_BUS_USE_WORKQUEUE
	if (ctx->wq)
		flush_workqueue(ctx->wq);
#endif

	if (ctx->works) {
		u32 num = ctx->num_urb;
		struct itedtv_usb_work *works = ctx->works;

		for (i = 0; i < num; i++)
			usb_kill_urb(works[i].urb);
	}

	itedtv_usb_clean_context(ctx, false);

	mutex_unlock(&ctx->lock);

	return 0;
}

int itedtv_bus_init(struct itedtv_bus *bus)
{
	int ret = 0;

	if (!bus)
		return -EINVAL;

	switch (bus->type) {
	case ITEDTV_BUS_USB:
	{
		struct itedtv_usb_context *ctx;

		if (!bus->usb.dev) {
			ret = -EINVAL;
			break;
		}

		if (bus->usb.dev->descriptor.bcdUSB < 0x0110) {
			ret = -EIO;
			break;
		}

		ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
		if (!ctx) {
			ret = -ENOMEM;
			break;
		}

		usb_get_dev(bus->usb.dev);

		mutex_init(&ctx->lock);
		ctx->bus = bus;
		ctx->stream_handler = NULL;
		ctx->ctx = NULL;
		ctx->num_urb = 0;
		ctx->no_dma = false;
#ifdef ITEDTV_BUS_USE_WORKQUEUE
		ctx->wq = NULL;
#endif
		ctx->num_works = 0;
		ctx->works = NULL;
		atomic_set(&ctx->streaming, 0);

		bus->usb.priv = ctx;

		if (!bus->usb.max_bulk_size)
			bus->usb.max_bulk_size = (bus->usb.dev->descriptor.bcdUSB == 0x0110) ? 64
											     : 512;

		bus->ops.ctrl_tx = itedtv_usb_ctrl_tx;
		bus->ops.ctrl_rx = itedtv_usb_ctrl_rx;
		bus->ops.stream_rx = itedtv_usb_stream_rx;
		bus->ops.start_streaming = itedtv_usb_start_streaming;
		bus->ops.stop_streaming = itedtv_usb_stop_streaming;

		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

int itedtv_bus_term(struct itedtv_bus *bus)
{
	int ret = 0;

	if (!bus) {
		ret = -EINVAL;
		goto exit;
	}

	switch (bus->type) {
	case ITEDTV_BUS_USB:
	{
		struct itedtv_usb_context *ctx = bus->usb.priv;

		if (ctx) {
			if (atomic_read_acquire(&ctx->streaming))
				itedtv_usb_stop_streaming(bus);

			itedtv_usb_clean_context(ctx, true);
			mutex_destroy(&ctx->lock);
			kfree(ctx);
		}

		if (bus->usb.dev)
			usb_put_dev(bus->usb.dev);

		break;
	}

	default:
		break;
	}

	memset(bus, 0, sizeof(*bus));

exit:
	return ret;
}
