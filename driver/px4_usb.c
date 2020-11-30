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
#include "pxmlt_device.h"
#include "isdb2056_device.h"

#ifndef PX4_USB_MAX_DEVICE
#define PX4_USB_MAX_DEVICE	16
#endif
#define PX4_USB_MAX_CHRDEV	(PX4_USB_MAX_DEVICE * PX4_CHRDEV_NUM)

#ifndef PXMLT5_USB_MAX_DEVICE
#define PXMLT5_USB_MAX_DEVICE	14
#endif
#define PXMLT5_USB_MAX_CHRDEV	(PXMLT5_USB_MAX_DEVICE * PXMLT5_CHRDEV_NUM)

#ifndef PXMLT8_USB_MAX_DEVICE
#define PXMLT8_USB_MAX_DEVICE	8
#endif
#define PXMLT8_USB_MAX_CHRDEV	(PXMLT8_USB_MAX_DEVICE * PXMLT8_CHRDEV_NUM)

#ifndef ISDB2056_USB_MAX_DEVICE
#define ISDB2056_USB_MAX_DEVICE		64
#endif
#define ISDB2056_USB_MAX_CHRDEV		(ISDB2056_USB_MAX_DEVICE * ISDB2056_CHRDEV_NUM)

#ifndef ISDB6014_USB_MAX_DEVICE
#define ISDB6014_USB_MAX_DEVICE		16
#endif
#define ISDB6014_USB_MAX_CHRDEV		(ISDB6014_USB_MAX_DEVICE * ISDB6014_CHRDEV_NUM)

struct px4_usb_context {
	enum px4_usb_device_type type;
	struct completion quit_completion;
	union {
		struct px4_device px4;
		struct pxmlt_device pxmlt;
		struct isdb2056_device isdb2056;
	} ctx;
};

static struct ptx_chrdev_context *px4_usb_chrdev_ctx[6];

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
		enum pxmlt_model pxmlt8_model = PXMLT8PE5_MODEL;

		switch (id->idProduct) {
		case USB_PID_PX_Q3U4:
		case USB_PID_PX_Q3PE4:
		case USB_PID_PX_Q3PE5:
			if (!px4_device_params.disable_multi_device_power_control)
				px4_use_mldev = true;
			/* fall through */
		case USB_PID_PX_W3U4:
		case USB_PID_PX_W3PE4:
		case USB_PID_PX_W3PE5:
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

		case USB_PID_PX_MLT5PE:
			ret = px4_usb_init_bridge(dev, usb_dev,
						  &ctx->ctx.pxmlt.it930x);
			if (ret)
				break;

			ctx->type = PXMLT5_USB_DEVICE;
			ret = pxmlt_device_init(&ctx->ctx.pxmlt, dev, PXMLT5PE_MODEL,
						px4_usb_chrdev_ctx[PXMLT5_USB_DEVICE],
						&ctx->quit_completion);
			break;

		case USB_PID_PX_MLT8PE3:
			pxmlt8_model = PXMLT8PE3_MODEL;
			/* fall through */
		case USB_PID_PX_MLT8PE5:
			ret = px4_usb_init_bridge(dev, usb_dev,
						  &ctx->ctx.pxmlt.it930x);
			if (ret)
				break;

			ctx->type = PXMLT8_USB_DEVICE;
			ret = pxmlt_device_init(&ctx->ctx.pxmlt, dev, pxmlt8_model,
						px4_usb_chrdev_ctx[PXMLT8_USB_DEVICE],
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

		case USB_PID_DIGIBEST_ISDB6014:
			ret = px4_usb_init_bridge(dev, usb_dev,
						  &ctx->ctx.pxmlt.it930x);
			if (ret)
				break;

			ctx->type = ISDB6014_USB_DEVICE;
			ret = pxmlt_device_init(&ctx->ctx.pxmlt, dev, ISDB6014_MODEL,
						px4_usb_chrdev_ctx[ISDB6014_USB_DEVICE],
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

	switch (ctx->type) {
	case PX4_USB_DEVICE:
		px4_device_term(&ctx->ctx.px4);
		wait_for_completion(&ctx->quit_completion);
		break;

	case PXMLT5_USB_DEVICE:
	case PXMLT8_USB_DEVICE:
	case ISDB6014_USB_DEVICE:
		pxmlt_device_term(&ctx->ctx.pxmlt);
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
	{ USB_DEVICE(0x0511, USB_PID_PX_W3PE5) },
	{ USB_DEVICE(0x0511, USB_PID_PX_Q3PE5) },
	{ USB_DEVICE(0x0511, USB_PID_PX_MLT5PE) },
	{ USB_DEVICE(0x0511, USB_PID_PX_MLT8PE3) },
	{ USB_DEVICE(0x0511, USB_PID_PX_MLT8PE5) },
	{ USB_DEVICE(0x0511, USB_PID_DIGIBEST_ISDB2056) },
	{ USB_DEVICE(0x0511, USB_PID_DIGIBEST_ISDB6014) },
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

	pr_debug("px4_usb_register: PX4_USB_MAX_DEVICE: %d\n", PX4_USB_MAX_DEVICE);
	pr_debug("px4_usb_register: PXMLT5_USB_MAX_DEVICE: %d\n", PXMLT5_USB_MAX_DEVICE);
	pr_debug("px4_usb_register: PXMLT8_USB_MAX_DEVICE: %d\n", PXMLT8_USB_MAX_DEVICE);
	pr_debug("px4_usb_register: ISDB2056_USB_MAX_DEVICE: %d\n", ISDB2056_USB_MAX_DEVICE);
	pr_debug("px4_usb_register: ISDB6014_USB_MAX_DEVICE: %d\n", ISDB6014_USB_MAX_DEVICE);

	memset(&px4_usb_chrdev_ctx, 0, sizeof(px4_usb_chrdev_ctx));

	ret = ptx_chrdev_context_create("px4", "px4video",
					PX4_USB_MAX_CHRDEV,
					&px4_usb_chrdev_ctx[PX4_USB_DEVICE]);
	if (ret) {
		pr_err("px4_usb_register: ptx_chrdev_context_create(\"px4\") failed.\n");
		goto fail;
	}

	ret = ptx_chrdev_context_create("pxmlt5", "pxmlt5video",
					PXMLT5_USB_MAX_CHRDEV,
					&px4_usb_chrdev_ctx[PXMLT5_USB_DEVICE]);
	if (ret) {
		pr_err("px4_usb_register: ptx_chrdev_context_create(\"pxmlt5\") failed.\n");
		goto fail_mlt5;
	}

	ret = ptx_chrdev_context_create("pxmlt8", "pxmlt8video",
					PXMLT8_USB_MAX_CHRDEV,
					&px4_usb_chrdev_ctx[PXMLT8_USB_DEVICE]);
	if (ret) {
		pr_err("px4_usb_register: ptx_chrdev_context_create(\"pxmlt8\") failed.\n");
		goto fail_mlt8;
	}

	ret = ptx_chrdev_context_create("isdb2056", "isdb2056video",
					ISDB2056_USB_MAX_CHRDEV,
					&px4_usb_chrdev_ctx[ISDB2056_USB_DEVICE]);
	if (ret) {
		pr_err("px4_usb_register: ptx_chrdev_context_create(\"isdb2056\") failed.\n");
		goto fail_isdb2056;
	}

	ret = ptx_chrdev_context_create("isdb6014", "isdb6014video",
					ISDB6014_USB_MAX_CHRDEV,
					&px4_usb_chrdev_ctx[ISDB6014_USB_DEVICE]);
	if (ret) {
		pr_err("px4_usb_register: ptx_chrdev_context_create(\"isdb6014\") failed.\n");
		goto fail_isdb6014;
	}

	ret = usb_register(&px4_usb_driver);
	if (ret) {
		pr_err("px4_usb_register: usb_register() failed.\n");
		goto fail_usb;
	}

	return 0;

fail_usb:
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[ISDB6014_USB_DEVICE]);

fail_isdb6014:
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[ISDB2056_USB_DEVICE]);

fail_isdb2056:
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PXMLT8_USB_DEVICE]);

fail_mlt8:
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PXMLT5_USB_DEVICE]);

fail_mlt5:
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PX4_USB_DEVICE]);

fail:
	return ret;
}

void px4_usb_unregister()
{
	usb_deregister(&px4_usb_driver);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[ISDB6014_USB_DEVICE]);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[ISDB2056_USB_DEVICE]);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PXMLT8_USB_DEVICE]);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PXMLT5_USB_DEVICE]);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PX4_USB_DEVICE]);
}
