// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver for USB devices (px4_usb.c)
 *
 * Copyright (c) 2018-2020 nns779
 */

#include "print_format.h"
#include "px4_usb.h"

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/usb.h>

#include "px4_usb_params.h"
#include "px4_device_params.h"
#include "ptx_chrdev.h"
#include "px4_device.h"
#include "isdb2056_device.h"

#define PX4_USB_MAX_DEVICE	16
#define PX4_USB_MAX_CHRDEV	(PX4_USB_MAX_DEVICE * PX4_CHRDEV_NUM)

#define ISDB2056_USB_MAX_DEVICE		64
#define ISDB2056_USB_MAX_CHRDEV		(ISDB2056_USB_MAX_DEVICE * ISDB2056_CHRDEV_NUM)

struct px4_usb_context {
	struct list_head list;
	enum px4_usb_device_type type;
	struct completion quit_completion;
	union {
		struct px4_device px4;
		struct isdb2056_device isdb2056;
	} ctx;
};

static DEFINE_MUTEX(px4_usb_glock);
static LIST_HEAD(px4_usb_ctx_list);
static struct ptx_chrdev_context *px4_usb_chrdev_ctx[7];

static int px4_usb_init_bridge(struct device *dev, struct usb_device *usb_dev,
			       struct it930x_bridge *it930x)
{
	struct itedtv_bus *bus = &it930x->bus;

	bus->dev = dev;
	bus->type = ITEDTV_BUS_USB;
	bus->usb.dev = usb_dev;
	bus->usb.ctrl_timeout = 3000;
	bus->usb.streaming.urb_buffer_size = 188 * px4_usb_params.urb_max_packets;
	bus->usb.streaming.urb_num = px4_usb_params.max_urbs;
	bus->usb.streaming.no_dma = px4_usb_params.no_dma;

	it930x->dev = dev;
	it930x->config.xfer_size = 188 * px4_usb_params.xfer_packets;
	it930x->config.i2c_speed = 0x07;

	return 0;
}

static int px4_usb_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	int ret = 0;
	struct device *dev;
	struct usb_device *usb_dev;
	struct px4_usb_context *ctx;

	dev = &intf->dev;
	usb_dev = interface_to_usbdev(intf);

	if (usb_dev->speed < USB_SPEED_HIGH)
		dev_warn(dev, "This device is operating as USB 1.1.\n");

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		dev_err(dev, "px4_usb_probe: kzalloc(sizeof(*ctx)) failed.\n");
		return -ENOMEM;
	}

	init_completion(&ctx->quit_completion);

	switch (id->idVendor) {
	case 0x0511:
	{
		bool px4_use_mldev = false;

		switch (id->idProduct) {
		case USB_PID_PX_Q3U4:
		case USB_PID_PX_Q3PE4:
			if (!px4_device_params.disable_multi_device_power_control)
				px4_use_mldev = true;
			/* fall through */
		case USB_PID_PX_W3U4:
		case USB_PID_PX_W3PE4:
			ret = px4_usb_init_bridge(dev, usb_dev,
						  &ctx->ctx.px4.it930x);
			if (ret)
				break;

			ctx->type = PX4_USB_DEVICE;
			ret = px4_device_init(&ctx->ctx.px4, dev,
					      usb_dev->serial, px4_use_mldev,
					      px4_usb_chrdev_ctx[PX4_USB_DEVICE],
					      &ctx->quit_completion);
			break;

		case USB_PID_DIGIBEST_ISDB2056:
			ret = px4_usb_init_bridge(dev, usb_dev,
						  &ctx->ctx.isdb2056.it930x);
			if (ret)
				break;

			ctx->type = ISDB2056_USB_DEVICE;
			ret = isdb2056_device_init(&ctx->ctx.isdb2056, dev,
						   px4_usb_chrdev_ctx[ISDB2056_USB_DEVICE],
						   &ctx->quit_completion);
			break;

		default:
			ret = -EINVAL;
			break;
		}

		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		goto fail;

	mutex_lock(&px4_usb_glock);
	list_add_tail(&ctx->list, &px4_usb_ctx_list);
	mutex_unlock(&px4_usb_glock);

	get_device(dev);
	usb_set_intfdata(intf, ctx);

	return 0;

fail:
	if (ctx)
		kfree(ctx);

	return ret;
}

static void px4_usb_disconnect(struct usb_interface *intf)
{
	struct px4_usb_context *ctx;

	ctx = usb_get_intfdata(intf);
	if (!ctx) {
		pr_err("px4_usb_disconnect: ctx is NULL.\n");
		return;
	}

	usb_set_intfdata(intf, NULL);

	mutex_lock(&px4_usb_glock);
	list_del(&ctx->list);
	mutex_unlock(&px4_usb_glock);

	switch (ctx->type) {
	case PX4_USB_DEVICE:
		px4_device_term(&ctx->ctx.px4);
		wait_for_completion(&ctx->quit_completion);
		break;

	case ISDB2056_USB_DEVICE:
		isdb2056_device_term(&ctx->ctx.isdb2056);
		wait_for_completion(&ctx->quit_completion);
		break;

	default:
		/* unknown device */
		break;
	}

	dev_dbg(&intf->dev, "px4_usb_disconnect: release\n");

	put_device(&intf->dev);
	kfree(ctx);

	return;
}

static int px4_usb_suspend(struct usb_interface *intf, pm_message_t message)
{
	return -ENOSYS;
}

static int px4_usb_resume(struct usb_interface *intf)
{
	return 0;
}

static const struct usb_device_id px4_usb_ids[] = {
	{ USB_DEVICE(0x0511, USB_PID_PX_W3U4) },
	{ USB_DEVICE(0x0511, USB_PID_PX_Q3U4) },
	{ USB_DEVICE(0x0511, USB_PID_PX_W3PE4) },
	{ USB_DEVICE(0x0511, USB_PID_PX_Q3PE4) },
	{ USB_DEVICE(0x0511, USB_PID_DIGIBEST_ISDB2056) },
	{ 0 }
};

MODULE_DEVICE_TABLE(usb, px4_usb_ids);

static struct usb_driver px4_usb_driver = {
	.name = "px4_usb",
	.probe = px4_usb_probe,
	.disconnect = px4_usb_disconnect,
	.suspend = px4_usb_suspend,
	.resume = px4_usb_resume,
	.id_table = px4_usb_ids
};

int px4_usb_register()
{
	int ret = 0;

	memset(&px4_usb_chrdev_ctx, 0, sizeof(px4_usb_chrdev_ctx));

	ret = ptx_chrdev_context_create("px4", "px4video",
					PX4_USB_MAX_CHRDEV,
					&px4_usb_chrdev_ctx[PX4_USB_DEVICE]);
	if (ret) {
		pr_err("px4_usb_register: ptx_chrdev_context_create() failed.\n");
		return ret;
	}

	ret = ptx_chrdev_context_create("isdb2056", "isdb2056video",
					ISDB2056_USB_MAX_CHRDEV,
					&px4_usb_chrdev_ctx[ISDB2056_USB_DEVICE]);
	if (ret) {
		pr_err("px4_usb_register: ptx_chrdev_context_create() failed.\n");
		ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PX4_USB_DEVICE]);
		return ret;
	}

	ret = usb_register(&px4_usb_driver);
	if (ret) {
		pr_err("px4_usb_register: usb_register() failed.\n");
		ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PX4_USB_DEVICE]);
		ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[ISDB2056_USB_DEVICE]);
		return ret;
	}

	return 0;
}

void px4_usb_unregister()
{
	usb_deregister(&px4_usb_driver);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PX4_USB_DEVICE]);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[ISDB2056_USB_DEVICE]);
}
