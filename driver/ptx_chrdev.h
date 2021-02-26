// SPDX-License-Identifier: GPL-2.0-only
/*
 * Character device operator definitions for PTX devices (ptx_chrdev.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __PTX_CHRDEV__
#define __PTX_CHRDEV__

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/cdev.h>
#include <linux/device.h>

#include "ptx_ioctl.h"
#include "ringbuffer.h"

struct ptx_tune_params {
	enum ptx_system_type system;
	u32 freq;
	u32 bandwidth;
	u16 stream_id;
};

struct ptx_chrdev;
struct ptx_chrdev_group;
struct ptx_chrdev_context;

struct ptx_chrdev_operations {
	int (*init)(struct ptx_chrdev *chrdev);
	int (*term)(struct ptx_chrdev *chrdev);
	int (*open)(struct ptx_chrdev *chrdev);
	int (*release)(struct ptx_chrdev *chrdev);
	int (*tune)(struct ptx_chrdev *chrdev, struct ptx_tune_params *params);
	int (*check_lock)(struct ptx_chrdev *chrdev, bool *locked);
	int (*set_stream_id)(struct ptx_chrdev *chrdev, u16 stream_id);
	int (*set_lnb_voltage)(struct ptx_chrdev *chrdev, int voltage);
	int (*set_capture)(struct ptx_chrdev *chrdev, bool status);
	int (*read_signal_strength)(struct ptx_chrdev *chrdev, u32 *value);
	int (*read_cnr)(struct ptx_chrdev *chrdev, u32 *value);
	int (*read_cnr_raw)(struct ptx_chrdev *chrdev, u32 *value);
};

#define PTX_CHRDEV_SAT_SET_STREAM_ID_BEFORE_TUNE	0x00000010
#define PTX_CHRDEV_SAT_SET_STREAM_ID_AFTER_TUNE		0x00000020
#define PTX_CHRDEV_WAIT_AFTER_LOCK			0x00000040
#define PTX_CHRDEV_WAIT_AFTER_LOCK_TC_T			0x00000080

struct ptx_chrdev_config {
	enum ptx_system_type system_cap;
	const struct ptx_chrdev_operations *ops;
	u32 options;
	size_t ringbuf_size;
	size_t ringbuf_threshold_size;
	void *priv;
};

struct ptx_chrdev_group_config {
	struct kref *owner_kref;
	void (*owner_kref_release)(struct kref *);
	bool reserved;
	unsigned int minor_base;
	unsigned int chrdev_num;
	struct ptx_chrdev_config *chrdev_config;
};

struct ptx_chrdev {
	struct mutex lock;
	unsigned int id;
	atomic_t open;
	char name[64];
	enum ptx_system_type system_cap;
	enum ptx_system_type current_system;
	const struct ptx_chrdev_operations *ops;
	struct ptx_chrdev_group *parent;
	struct ptx_tune_params params;
	u32 options;
	bool streaming;
	struct ringbuffer *ringbuf;
	wait_queue_head_t ringbuf_wait;
	size_t ringbuf_threshold_size;
	size_t ringbuf_write_size;
	void *priv;
};

struct ptx_chrdev_group {
	struct list_head list;
	struct mutex lock;
	struct kref kref;
	unsigned int id;
	atomic_t available;
	struct ptx_chrdev_context *parent;
	struct device *dev;
	struct cdev cdev;
	struct kref *owner_kref;
	void (*owner_kref_release)(struct kref *);
	unsigned int minor_base;
	unsigned int chrdev_num;
	struct ptx_chrdev chrdev[1];
};

#define PTX_CHRDEV_MINOR_FREE		0
#define PTX_CHRDEV_MINOR_RESERVED	1
#define PTX_CHRDEV_MINOR_IN_USE		2

struct ptx_chrdev_context {
	struct list_head list;
	struct mutex lock;
	struct kref kref;
	char devname[64];
	struct class *class;
	dev_t dev_base;
	unsigned int last_id;
	unsigned int minor_num;
	u8 *minor_table;
	struct list_head group_list;
};

int ptx_chrdev_context_create(const char *name, const char *devname,
			      unsigned int total_num,
			      struct ptx_chrdev_context **chrdev_ctx);
void ptx_chrdev_context_destroy(struct ptx_chrdev_context *chrdev_ctx);
int ptx_chrdev_context_add_group(struct ptx_chrdev_context *chrdev_ctx,
				 struct device *dev,
				 const struct ptx_chrdev_group_config *config,
				 struct ptx_chrdev_group **chrdev_group);
int ptx_chrdev_context_remove_group(struct ptx_chrdev_context *chrdev_ctx,
				    unsigned int minor_base);
void ptx_chrdev_group_destroy(struct ptx_chrdev_group *chrdev_group);
int ptx_chrdev_put_stream(struct ptx_chrdev *chrdev, void *buf, size_t len);

#endif
