// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTX driver definitions for Digibest ISDB2056 device (isdb2056_device.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __ISDB2056_DEVICE_H__
#define __ISDB2056_DEVICE_H__

#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/device.h>

#include "ptx_chrdev.h"
#include "px4_card.h"
#include "it930x.h"
#include "tc90522.h"
#include "r850.h"
#include "rt710.h"

#define ISDB2056_CHRDEV_NUM	1

enum isdb2056_model {
	ISDB2056_MODEL = 0,
	ISDB2056N_MODEL,
	/* PX-M1UR は ISDB2056 と同じ構成 (復調器・チューナー・GPIO) で、デバイスファイル名だけが異なる */
	PXM1UR_MODEL,
	/* PX-S1UR は衛星チューナーを持たない地デジ専用機種 */
	PXS1UR_MODEL,
};

struct isdb2056_chrdev {
	struct ptx_chrdev *chrdev;
	struct tc90522_demod tc90522_t;
	struct tc90522_demod tc90522_s;
	struct tc90522_demod tc90522_s0;
	struct r850_tuner r850;
	struct rt710_tuner rt710;
};

struct isdb2056_device {
	struct kref kref;
	/* チューナーと内蔵カードリーダーで共有する基板電源の状態を守る */
	struct mutex lock;
	atomic_t available;
	bool tuner_open;
	/* 内蔵カードリーダーが基板の電源を使っているか (チューナーを閉じても、カードの使用中は電源を切らない) */
	bool card_powered;
	struct px4_card *card;
	struct device *dev;
	enum isdb2056_model isdb2056_model;
	struct completion *quit_completion;
	struct ptx_chrdev_group *chrdev_group;
	struct isdb2056_chrdev chrdev2056;
	struct it930x_bridge it930x;
	void *stream_ctx;
};

int isdb2056_device_init(struct isdb2056_device *isdb2056, struct device *dev,
			 enum isdb2056_model isdb2056_model,
			 struct ptx_chrdev_context *chrdev_ctx,
			 struct completion *quit_completion);
void isdb2056_device_term(struct isdb2056_device *isdb2056);

#endif
