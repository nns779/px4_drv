// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver for USB devices (px4_usb.c)
 *
 * Copyright (c) 2018-2021 nns779
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
#include "s1ur_device.h"
#include "m1ur_device.h"

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

#ifndef ISDB6014_4TS_USB_MAX_DEVICE
#define ISDB6014_4TS_USB_MAX_DEVICE	16
#endif
#define ISDB6014_4TS_USB_MAX_CHRDEV	(ISDB6014_4TS_USB_MAX_DEVICE * ISDB6014_4TS_CHRDEV_NUM)

#ifndef PXM1UR_USB_MAX_DEVICE
#define PXM1UR_USB_MAX_DEVICE	64
#endif
#define PXM1UR_USB_MAX_CHRDEV	(PXM1UR_USB_MAX_DEVICE * M1UR_CHRDEV_NUM)

#ifndef PXS1UR_USB_MAX_DEVICE
#define PXS1UR_USB_MAX_DEVICE	64
#endif
#define PXS1UR_USB_MAX_CHRDEV	(PXS1UR_USB_MAX_DEVICE * S1UR_CHRDEV_NUM)


struct px4_usb_context {
	enum px4_usb_device_type type;
	struct completion quit_completion;
	union {
		struct px4_device px4;
		struct pxmlt_device pxmlt;
		struct isdb2056_device isdb2056;
		struct m1ur_device m1ur;
		struct s1ur_device s1ur;
	} ctx;
};

static struct ptx_chrdev_context *px4_usb_chrdev_ctx[MAX_USB_DEVICE_TYPE];

static int px4_usb_init_bridge(struct device *dev, struct usb_device *usb_dev,
			       struct it930x_bridge *it930x)
{
	struct itedtv_bus *bus = &it930x->bus;

	bus->dev = dev;
	bus->type = ITEDTV_BUS_USB;
	bus->usb.dev = usb_dev;
	bus->usb.ctrl_timeout = px4_usb_params.ctrl_timeout;
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
		enum pxmlt_model pxmlt5_model = PXMLT5PE_MODEL;
		enum pxmlt_model pxmlt8_model = PXMLT8PE5_MODEL;

		switch (id->idProduct) {
		case USB_PID_PX_Q3U4:
		case USB_PID_PX_Q3PE4:
		case USB_PID_PX_Q3PE5:
			if (!px4_device_params.disable_multi_device_power_control)
				px4_use_mldev = true;

			dev_info(dev, "Multi-device power control: %s\n",
				 (px4_use_mldev) ? "enabled" : "disabled");
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

		case USB_PID_PX_MLT5U:
			pxmlt5_model = PXMLT5U_MODEL;
			/* fall through */
		case USB_PID_PX_MLT5PE:
			ret = px4_usb_init_bridge(dev, usb_dev,
						  &ctx->ctx.pxmlt.it930x);
			if (ret)
				break;

			ctx->type = PXMLT5_USB_DEVICE;
			ret = pxmlt_device_init(&ctx->ctx.pxmlt, dev, pxmlt5_model,
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
			ret = isdb2056_device_init(&ctx->ctx.isdb2056, dev, ISDB2056_MODEL,
						   px4_usb_chrdev_ctx[ISDB2056_USB_DEVICE],
						   &ctx->quit_completion);
			break;

		case USB_PID_DIGIBEST_ISDB2056N:
			ret = px4_usb_init_bridge(dev, usb_dev,
						  &ctx->ctx.isdb2056.it930x);
			if (ret)
				break;

			ctx->type = ISDB2056_USB_DEVICE;
			ret = isdb2056_device_init(&ctx->ctx.isdb2056, dev, ISDB2056N_MODEL,
						   px4_usb_chrdev_ctx[ISDB2056_USB_DEVICE],
						   &ctx->quit_completion);
			break;

		case USB_PID_DIGIBEST_ISDB6014_4TS:
			ret = px4_usb_init_bridge(dev, usb_dev,
						  &ctx->ctx.pxmlt.it930x);
			if (ret)
				break;

			ctx->type = ISDB6014_4TS_USB_DEVICE;
			ret = pxmlt_device_init(&ctx->ctx.pxmlt, dev, ISDB6014_4TS_MODEL,
						px4_usb_chrdev_ctx[ISDB6014_4TS_USB_DEVICE],
						&ctx->quit_completion);
			break;

		case USB_PID_PX_M1UR:
			ret = px4_usb_init_bridge(dev, usb_dev,
						  &ctx->ctx.m1ur.it930x);
			if (ret)
				break;

			ctx->type = PXM1UR_USB_DEVICE;
			ret = m1ur_device_init(&ctx->ctx.m1ur, dev,
						   px4_usb_chrdev_ctx[PXM1UR_USB_DEVICE],
						   &ctx->quit_completion);
			break;
		
		case USB_PID_PX_S1UR:
			ret = px4_usb_init_bridge(dev, usb_dev,
						  &ctx->ctx.s1ur.it930x);
			if (ret)
				break;

			ctx->type = PXS1UR_USB_DEVICE;
			ret = s1ur_device_init(&ctx->ctx.s1ur, dev,
						   px4_usb_chrdev_ctx[PXS1UR_USB_DEVICE],
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
	case ISDB6014_4TS_USB_DEVICE:
		pxmlt_device_term(&ctx->ctx.pxmlt);
		wait_for_completion(&ctx->quit_completion);
		break;

	case ISDB2056_USB_DEVICE:
		isdb2056_device_term(&ctx->ctx.isdb2056);
		wait_for_completion(&ctx->quit_completion);
		break;
	
	case PXM1UR_USB_DEVICE:
		m1ur_device_term(&ctx->ctx.m1ur);
		wait_for_completion(&ctx->quit_completion);
		break;

	case PXS1UR_USB_DEVICE:
		s1ur_device_term(&ctx->ctx.s1ur);
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
	{ USB_DEVICE(0x0511, USB_PID_PX_MLT5U) },
	{ USB_DEVICE(0x0511, USB_PID_PX_MLT5PE) },
	{ USB_DEVICE(0x0511, USB_PID_PX_MLT8PE3) },
	{ USB_DEVICE(0x0511, USB_PID_PX_MLT8PE5) },
	{ USB_DEVICE(0x0511, USB_PID_DIGIBEST_ISDB2056) },
	{ USB_DEVICE(0x0511, USB_PID_DIGIBEST_ISDB2056N) },
	{ USB_DEVICE(0x0511, USB_PID_DIGIBEST_ISDB6014_4TS) },
	{ USB_DEVICE(0x0511, USB_PID_PX_M1UR) },
	{ USB_DEVICE(0x0511, USB_PID_PX_S1UR) },
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
	pr_debug("px4_usb_register: ISDB6014_4TS_USB_MAX_DEVICE: %d\n", ISDB6014_4TS_USB_MAX_DEVICE);
	pr_debug("px4_usb_register: PXM1UR_USB_MAX_DEVICE: %d\n", PXM1UR_USB_MAX_DEVICE);
	pr_debug("px4_usb_register: PXS1UR_USB_MAX_DEVICE: %d\n", PXS1UR_USB_MAX_DEVICE);

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
					ISDB6014_4TS_USB_MAX_CHRDEV,
					&px4_usb_chrdev_ctx[ISDB6014_4TS_USB_DEVICE]);
	if (ret) {
		pr_err("px4_usb_register: ptx_chrdev_context_create(\"isdb6014\") failed.\n");
		goto fail_isdb6014;
	}

	ret = ptx_chrdev_context_create("pxm1ur", "pxm1urvideo",
					PXM1UR_USB_MAX_CHRDEV,
					&px4_usb_chrdev_ctx[PXM1UR_USB_DEVICE]);
	if (ret) {
		pr_err("px4_usb_register: ptx_chrdev_context_create(\"pxm1ur\") failed.\n");
		goto fail_pxm1ur;
	}

	ret = ptx_chrdev_context_create("pxs1ur", "pxs1urvideo",
					PXS1UR_USB_MAX_CHRDEV,
					&px4_usb_chrdev_ctx[PXS1UR_USB_DEVICE]);
	if (ret) {
		pr_err("px4_usb_register: ptx_chrdev_context_create(\"pxs1ur\") failed.\n");
		goto fail_pxs1ur;
	}

	ret = usb_register(&px4_usb_driver);
	if (ret) {
		pr_err("px4_usb_register: usb_register() failed.\n");
		goto fail_usb;
	}

	return 0;

fail_usb:
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[ISDB6014_4TS_USB_DEVICE]);

fail_pxs1ur:
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PXS1UR_USB_DEVICE]);

fail_pxm1ur:
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PXM1UR_USB_DEVICE]);

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
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PXS1UR_USB_DEVICE]);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PXM1UR_USB_DEVICE]);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[ISDB6014_4TS_USB_DEVICE]);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[ISDB2056_USB_DEVICE]);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PXMLT8_USB_DEVICE]);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PXMLT5_USB_DEVICE]);
	ptx_chrdev_context_destroy(px4_usb_chrdev_ctx[PX4_USB_DEVICE]);
}
