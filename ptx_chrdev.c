// SPDX-License-Identifier: GPL-2.0-only
/*
 * Character device operator for PTX devices (ptx_chrdev.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "print_format.h"
#include "ptx_chrdev.h"
#include "isdb2056_device.h"

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/version.h>

static LIST_HEAD(ctx_list);
static DEFINE_MUTEX(ctx_list_lock);

static bool ptx_chrdev_search_context(unsigned int major,
				      struct ptx_chrdev_context **chrdev_ctx);
static bool ptx_chrdev_context_search_group(struct ptx_chrdev_context *chrdev_ctx,
					    unsigned int minor,
					    struct ptx_chrdev_group **chrdev_group);
static void ptx_chrdev_group_release(struct kref *kref);
static void ptx_chrdev_context_release(struct kref *kref);

static int ptx_chrdev_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	unsigned int major, minor;
	struct ptx_chrdev_context *ctx;
	struct ptx_chrdev_group *group;
	struct ptx_chrdev *chrdev = NULL;
	struct kref *owner_kref = NULL;
	void (*owner_kref_release)(struct kref *) = NULL;

	major = imajor(inode);
	minor = iminor(inode);

	mutex_lock(&ctx_list_lock);

	if (!ptx_chrdev_search_context(major, &ctx)) {
		mutex_unlock(&ctx_list_lock);
		ret = -ENOENT;
		goto fail;
	}

	kref_get(&ctx->kref);
	mutex_lock(&ctx->lock);
	mutex_unlock(&ctx_list_lock);

	if (!ptx_chrdev_context_search_group(ctx, minor, &group)) {
		mutex_unlock(&ctx->lock);
		ret = -ENOENT;
		goto fail_ctx;
	}

	owner_kref = group->owner_kref;
	owner_kref_release = group->owner_kref_release;

	if (owner_kref)
		kref_get(owner_kref);

	kref_get(&group->kref);
	mutex_lock(&group->lock);
	mutex_unlock(&ctx->lock);

	if (!atomic_read(&group->available)) {
		mutex_unlock(&group->lock);
		ret = -ENOENT;
		goto fail_group;
	}

	chrdev = &group->chrdev[minor - group->minor_base];

	ret = (atomic_cmpxchg(&chrdev->open, 0, 1)) ? -EALREADY : 0;
	if (ret) {
		mutex_unlock(&group->lock);
		goto fail_group;
	}

	mutex_lock(&chrdev->lock);
	mutex_unlock(&group->lock);

	chrdev->current_system = PTX_UNSPECIFIED_SYSTEM;

	if (chrdev->ops && chrdev->ops->open)
		ret = chrdev->ops->open(chrdev);

	if (!ret)
		file->private_data = chrdev;

	mutex_unlock(&chrdev->lock);

	if (ret)
		goto fail_chrdev;

	return 0;

fail_chrdev:
	atomic_dec_return(&chrdev->open);

fail_group:
	kref_put(&group->kref, ptx_chrdev_group_release);

	if (owner_kref)
		kref_put(owner_kref, owner_kref_release);

fail_ctx:
	kref_put(&ctx->kref, ptx_chrdev_context_release);

fail:
	return ret;
}

static ssize_t ptx_chrdev_read(struct file *file,
			       char __user *buf, size_t count, loff_t *ppos)
{
	int ret = 0;
	struct ptx_chrdev *chrdev = file->private_data;
	struct ptx_chrdev_group *group = chrdev->parent;
	u8 __user *p = buf;
	size_t remain = count;

	if (unlikely(!atomic_read_acquire(&group->available)))
		return -EIO;

	ringbuffer_ready_read(chrdev->ringbuf);

	while (likely(remain)) {
		size_t len;

		if (wait_event_interruptible(chrdev->ringbuf_wait,
					     likely(ringbuffer_is_readable(chrdev->ringbuf)) ||
					     unlikely(!ringbuffer_is_running(chrdev->ringbuf)) ||
					     unlikely(!atomic_read(&group->available)))) {
			if (unlikely(remain == count))
				ret = -EINTR;

			break;
		}

		len = remain;
		ret = ringbuffer_read_user(chrdev->ringbuf, p, &len);
		if (unlikely(ret || !len))
			break;

		p += len;
		remain -= len;
	}

	return likely(!ret) ? (count - remain) : ret;
}

static int ptx_chrdev_release(struct inode *inode, struct file *file)
{
	int ret = 0;
	struct ptx_chrdev *chrdev = file->private_data;
	struct ptx_chrdev_group *group = chrdev->parent;
	struct ptx_chrdev_context *ctx = group->parent;
	struct kref *owner_kref = group->owner_kref;
	void (*owner_kref_release)(struct kref *) = group->owner_kref_release;

	mutex_lock(&chrdev->lock);

	if (chrdev->streaming) {
		if (chrdev->ops && chrdev->ops->set_capture)
			chrdev->ops->set_capture(chrdev, false);

		ringbuffer_stop(chrdev->ringbuf);
		wake_up(&chrdev->ringbuf_wait);
		chrdev->streaming = false;
	}

	if (chrdev->ops && chrdev->ops->release)
		ret = chrdev->ops->release(chrdev);

	mutex_unlock(&chrdev->lock);

	atomic_dec_return(&chrdev->open);
	kref_put(&group->kref, ptx_chrdev_group_release);

	if (owner_kref)
		kref_put(owner_kref, owner_kref_release);

	kref_put(&ctx->kref, ptx_chrdev_context_release);
	return ret;
}

static long ptx_chrdev_unlocked_ioctl(struct file *file,
				      unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct ptx_chrdev *chrdev = file->private_data;
	struct ptx_chrdev_group *group = chrdev->parent;

	if (!atomic_read_acquire(&group->available))
		return -EIO;

	mutex_lock(&chrdev->lock);

	switch (cmd) {
	case PTX_SET_CHANNEL:
	{
		struct ptx_freq freq;
		enum ptx_system_type system;

		if (!chrdev->ops || !chrdev->ops->tune) {
			ret = -ENOSYS;
			break;
		}

		if (copy_from_user(&freq, (void *)arg, sizeof(freq))) {
			ret = -EFAULT;
			break;
		}

		system = chrdev->params.system;

		switch (chrdev->params.system) {
		case PTX_ISDB_S_SYSTEM:
			if (freq.freq_no < 0) {
				ret = -EINVAL;
				break;
			} else if (freq.freq_no < 12) {
				/* BS */
				if (0 && freq.slot >= 8) {
					ret = -EINVAL;
					break;
				}
				chrdev->params.freq = 1049480 + (38360 * freq.freq_no);
			} else if (freq.freq_no < 24) {
				/* CS */
				chrdev->params.freq = 1613000 + (40000 * (freq.freq_no - 12));
			} else {
				ret = -EINVAL;
				break;
			}
			chrdev->params.bandwidth = 0;
			chrdev->params.stream_id = freq.slot;
			break;

		case PTX_ISDB_T_SYSTEM:
			if ((freq.freq_no >= 3 && freq.freq_no <= 12) ||
			    (freq.freq_no >= 22 && freq.freq_no <= 62)) {
				/* CATV C13-C22ch, C23-C63ch */
				chrdev->params.freq = 93143 + freq.freq_no * 6000 + freq.slot/* addfreq */;

				if (freq.freq_no == 12)
					chrdev->params.freq += 2000;
			} else if (freq.freq_no >= 63 && freq.freq_no <= 112) {
				/* UHF 13-62ch */
				chrdev->params.freq = 95143 + freq.freq_no * 6000 + freq.slot/* addfreq */;
			} else {
				ret = -EINVAL;
				break;
			}
			chrdev->params.bandwidth = 6;
			chrdev->params.stream_id = 0;
			break;

		case PTX_UNSPECIFIED_SYSTEM:
			if (chrdev->system_cap & PTX_ISDB_S_SYSTEM) {
				if (freq.freq_no < 0) {
					ret = -EINVAL;
					break;
				} else if (freq.freq_no < 12) {
					/* BS */
					if (0 && freq.slot >= 8) {
						ret = -EINVAL;
						break;
					}
					chrdev->params.freq = 1049480 + (38360 * freq.freq_no);
					chrdev->params.bandwidth = 0;
					chrdev->params.stream_id = freq.slot;
					chrdev->params.system = PTX_ISDB_S_SYSTEM;
					break;
				} else if (freq.freq_no < 24) {
					/* CS */
					chrdev->params.freq = 1613000 + (40000 * (freq.freq_no - 12));
					chrdev->params.bandwidth = 0;
					chrdev->params.stream_id = freq.slot;
					chrdev->params.system = PTX_ISDB_S_SYSTEM;
					break;
				}
			}

			if (chrdev->system_cap & PTX_ISDB_T_SYSTEM) {
				if (freq.freq_no >= 24 && freq.freq_no <= 62) {
					/* CATV C25-C63ch */
					chrdev->params.freq = 93143 + freq.freq_no * 6000 + freq.slot/* addfreq */;
					chrdev->params.bandwidth = 6;
					chrdev->params.stream_id = 0;
					chrdev->params.system = PTX_ISDB_T_SYSTEM;
					break;
				} else if (freq.freq_no >= 63 && freq.freq_no <= 112) {
					/* UHF 13-62ch */
					chrdev->params.freq = 95143 + freq.freq_no * 6000 + freq.slot/* addfreq */;
					chrdev->params.bandwidth = 6;
					chrdev->params.stream_id = 0;
					chrdev->params.system = PTX_ISDB_T_SYSTEM;
					break;
				}
			}

			ret = -EINVAL;
			break;

		default:
			ret = -ENOSYS;
			break;
		}

		if (ret)
			break;

		if (chrdev->params.system == PTX_ISDB_S_SYSTEM &&
		    (chrdev->options & PTX_CHRDEV_SAT_SET_STREAM_ID_BEFORE_TUNE) &&
		    chrdev->ops->set_stream_id) {
			ret = chrdev->ops->set_stream_id(chrdev,
							 chrdev->params.stream_id);
			if (ret)
				break;
		}

		ret = chrdev->ops->tune(chrdev, &chrdev->params);
		if (ret) {
			chrdev->params.system = system;
			break;
		}

		chrdev->current_system = chrdev->params.system;
		chrdev->params.system = system;

		if (chrdev->ops->check_lock) {
			int i;
			bool locked = false;

			i = 300;
			while (i--) {
				ret = chrdev->ops->check_lock(chrdev, &locked);
				if ((!ret && locked) || ret == -ECANCELED)
					break;

				msleep(10);
			}

			if (ret != -ECANCELED && !locked)
				ret = -EAGAIN;

			if (ret)
				break;

			if (chrdev->current_system == PTX_ISDB_T_SYSTEM &&
			    (chrdev->options & PTX_CHRDEV_WAIT_AFTER_LOCK_TC_T) &&
			    i > 265)
				msleep((i - 265) * 10);
		}

		if (chrdev->current_system == PTX_ISDB_S_SYSTEM &&
		    !(chrdev->options & PTX_CHRDEV_SAT_SET_STREAM_ID_BEFORE_TUNE) &&
		    chrdev->ops->set_stream_id)
			ret = chrdev->ops->set_stream_id(chrdev,
							 chrdev->params.stream_id);

		if (chrdev->options & PTX_CHRDEV_WAIT_AFTER_LOCK)
			msleep(200);

		break;
	}

	case PTX_START_STREAMING:
		if (chrdev->streaming) {
			ret = -EALREADY;
			break;
		}

		chrdev->ringbuf_write_size = 0;

		if (chrdev->ops && chrdev->ops->set_capture)
			ret = chrdev->ops->set_capture(chrdev, true);
		else
			ret = -ENOSYS;

		if (!ret) {
			ringbuffer_reset(chrdev->ringbuf);
			ringbuffer_start(chrdev->ringbuf);
			chrdev->streaming = true;
		}

		break;

	case PTX_STOP_STREAMING:
		if (!chrdev->streaming) {
			ret = -EALREADY;
			break;
		}

		if (chrdev->ops && chrdev->ops->set_capture)
			ret = chrdev->ops->set_capture(chrdev, false);
		else
			ret = -ENOSYS;

		if (!ret) {
			ringbuffer_stop(chrdev->ringbuf);
			wake_up(&chrdev->ringbuf_wait);
			chrdev->streaming = false;
		}

		break;

	case PTX_GET_CNR:
	{
		u32 cn = 0;

		if (chrdev->ops && chrdev->ops->read_cnr_raw)
			ret = chrdev->ops->read_cnr_raw(chrdev, &cn);
		else
			ret = -ENOSYS;

		if (ret)
			break;

		if (copy_to_user((void *)arg, &cn, sizeof(cn)))
			ret = -EFAULT;

		break;
	}

	case PTX_ENABLE_LNB_POWER:
		if (chrdev->ops && chrdev->ops->set_lnb_voltage) {
			int voltage;

			switch (arg) {
			case 0:
				voltage = 0;
				break;

			case 1:
				voltage = 11;
				break;

			case 2:
				voltage = 15;
				break;

			default:
				ret = -EINVAL;
				break;
			}

			if (ret)
				break;

			ret = chrdev->ops->set_lnb_voltage(chrdev, voltage);
		} else if (arg) {
			ret = -ENOSYS;
		}

		break;

	case PTX_DISABLE_LNB_POWER:
		if (chrdev->ops && chrdev->ops->set_lnb_voltage)
			ret = chrdev->ops->set_lnb_voltage(chrdev, 0);

		break;

	case PTX_SET_SYSTEM_MODE:
	{
		enum ptx_system_type mode = (enum ptx_system_type)arg;

		switch (mode) {
		case PTX_UNSPECIFIED_SYSTEM:
		case PTX_ISDB_T_SYSTEM:
		case PTX_ISDB_S_SYSTEM:
			if (chrdev->system_cap & mode)
				chrdev->params.system = mode;
			else
				ret = -EINVAL;
			break;

		default:
			ret = -EINVAL;
			break;
		}

		break;
	}

#if 0
	case PTXT_GET_INFO:
		break;

	case PTXT_GET_PARAMS:
		break;

	case PTXT_SET_PARAMS:
		break;

	case PTXT_CLEAR_PARAMS:
		break;

	case PTXT_TUNE:
		break;

	case PTXT_SET_LNB_VOLTAGE:
		break;

	case PTXT_SET_CAPTURE:
		break;

	case PTXT_READ_STATS:
		break;
#endif

	default:
		ret = -ENOSYS;
		break;
	}

	mutex_unlock(&chrdev->lock);
	return ret;
}

static struct file_operations ptx_chrdev_fops = {
	.owner = THIS_MODULE,
	.open = ptx_chrdev_open,
	.read = ptx_chrdev_read,
	.release = ptx_chrdev_release,
	.unlocked_ioctl = ptx_chrdev_unlocked_ioctl
};

static bool ptx_chrdev_search_context(unsigned int major,
				      struct ptx_chrdev_context **chrdev_ctx)
{
	struct ptx_chrdev_context *ctx;

	*chrdev_ctx = NULL;

	list_for_each_entry(ctx, &ctx_list, list) {
		if (MAJOR(ctx->dev_base) != major)
			continue;

		*chrdev_ctx = ctx;
		break;
	}

	return (*chrdev_ctx) ? true : false;
}

int ptx_chrdev_context_create(const char *name, const char *devname,
			      unsigned int total_num,
			      struct ptx_chrdev_context **chrdev_ctx)
{
	int ret = 0;
	struct ptx_chrdev_context *ctx;

	if (!name || !devname || !total_num || !chrdev_ctx)
		return -EINVAL;

	ctx = kzalloc(sizeof(*ctx) + (sizeof(*ctx->minor_table) * total_num),
		      GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,8,0)
	strscpy(ctx->devname, devname, sizeof(ctx->devname));
#else
	strlcpy(ctx->devname, devname, sizeof(ctx->devname));
#endif

	INIT_LIST_HEAD(&ctx->group_list);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
	ctx->class = class_create(name);
#else
	ctx->class = class_create(THIS_MODULE, name);
#endif
	if (IS_ERR(ctx->class)) {
		pr_err("ptx_chrdev_context_create: class_create(\"%s\") failed.\n",
		       name);
		kfree(ctx);
		return PTR_ERR(ctx->class);
	}

	ret = alloc_chrdev_region(&ctx->dev_base, 0, total_num, name);
	if (ret < 0) {
		pr_err("ptx_chrdev_context_create: alloc_chrdev_region(\"%s\") failed.\n",
		       name);
		class_destroy(ctx->class);
		kfree(ctx);
		return ret;
	}

	kref_init(&ctx->kref);
	ctx->last_id = 0;
	ctx->minor_num = total_num;
	ctx->minor_table = ((u8 *)ctx) + sizeof(*ctx);

	mutex_lock(&ctx_list_lock);
	list_add_tail(&ctx->list, &ctx_list);
	mutex_unlock(&ctx_list_lock);

	*chrdev_ctx = ctx;

	return 0;
}

static void ptx_chrdev_context_release(struct kref *kref)
{
	struct ptx_chrdev_context *ctx = container_of(kref,
						      struct ptx_chrdev_context,
						      kref);

	pr_debug("ptx_chrdev_context_release\n");

	unregister_chrdev_region(ctx->dev_base, ctx->minor_num);
	class_destroy(ctx->class);
	mutex_destroy(&ctx->lock);
	kfree(ctx);

	return;
}

void ptx_chrdev_context_destroy(struct ptx_chrdev_context *chrdev_ctx)
{
	struct ptx_chrdev_group *group, *tmp_group;

	mutex_lock(&ctx_list_lock);
	list_del(&chrdev_ctx->list);
	mutex_unlock(&ctx_list_lock);

	mutex_lock(&chrdev_ctx->lock);
	list_for_each_entry_safe(group, tmp_group,
				 &chrdev_ctx->group_list, list) {
		ptx_chrdev_group_destroy(group);
	}
	mutex_unlock(&chrdev_ctx->lock);

	kref_put(&chrdev_ctx->kref, ptx_chrdev_context_release);
	return;
}

static bool ptx_chrdev_context_search_group(struct ptx_chrdev_context *chrdev_ctx,
					    unsigned int minor,
					    struct ptx_chrdev_group **chrdev_group)
{
	struct ptx_chrdev_group *group;

	*chrdev_group = NULL;

	list_for_each_entry(group, &chrdev_ctx->group_list, list) {
		if (group->minor_base > minor ||
		    minor >= (group->minor_base + group->chrdev_num))
			continue;

		*chrdev_group = group;
		break;
	}

	return (*chrdev_group) ? true : false;
}

static int ptx_chrdev_context_search_minor(struct ptx_chrdev_context *chrdev_ctx,
					   unsigned int num, u8 state,
					   unsigned int *base)
{
	unsigned int i, j;

	if (num > chrdev_ctx->minor_num)
		return -EINVAL;

	for (i = 0; i < (chrdev_ctx->minor_num - num + 1); i++) {
		if (chrdev_ctx->minor_table[i] != state)
			continue;

		for (j = 1; j < num; j++) {
			if (chrdev_ctx->minor_table[i + j] != state)
				break;
		}
		if (j < num) {
			i += j;
			continue;
		}

		/* found */
		*base = i;
		return 0;
	}

	return -EBUSY;
}

static int ptx_chrdev_context_check_minor_status(struct ptx_chrdev_context *chrdev_ctx,
						 unsigned int base,
						 unsigned int num,
						 u8 state,bool *res)
{
	unsigned int i;

	if ((base + num) > chrdev_ctx->minor_num)
		return -EINVAL;

	*res = true;

	for (i = 0; i < num; i++) {
		if (chrdev_ctx->minor_table[base + i] != state) {
			*res = false;
			break;
		}
	}

	return 0;
}

static int ptx_chrdev_context_set_minor_status(struct ptx_chrdev_context *chrdev_ctx,
					       unsigned int base,
					       unsigned int num,
					       u8 state)
{
	unsigned int i;

	if ((base + num) > chrdev_ctx->minor_num)
		return -EINVAL;

	for (i = 0; i < num; i++)
		chrdev_ctx->minor_table[base + i] = state;

	return 0;
}

int ptx_chrdev_context_reserve(struct ptx_chrdev_context *chrdev_ctx,
			       unsigned int num, unsigned int *minor_base)
{
	int ret = 0;
	unsigned int base;

	mutex_lock(&chrdev_ctx->lock);

	ret = ptx_chrdev_context_search_minor(chrdev_ctx, num,
					      PTX_CHRDEV_MINOR_FREE, &base);
	if (ret)
		goto exit;

	ptx_chrdev_context_set_minor_status(chrdev_ctx, base, num,
					    PTX_CHRDEV_MINOR_RESERVED);
	*minor_base = MINOR(chrdev_ctx->dev_base) + base;

exit:
	mutex_unlock(&chrdev_ctx->lock);
	return ret;
}

int ptx_chrdev_context_add_group(struct ptx_chrdev_context *chrdev_ctx,
				 struct device *dev,
				 const struct ptx_chrdev_group_config *config,
				 struct ptx_chrdev_group **chrdev_group)
{
	int ret = 0;
	unsigned int i, num, base;
	struct ptx_chrdev_group *group = NULL;

	if (!chrdev_ctx || !dev || !config)
		return -EINVAL;

	num = config->chrdev_num;

	kref_get(&chrdev_ctx->kref);
	mutex_lock(&chrdev_ctx->lock);

	if (config->owner_kref)
		kref_get(config->owner_kref);

	if (config->reserved) {
		bool res;

		base = config->minor_base - MINOR(chrdev_ctx->dev_base);
		ret = ptx_chrdev_context_check_minor_status(chrdev_ctx,
							    base, num,
							    PTX_CHRDEV_MINOR_RESERVED,
							    &res);
		if (!ret && !res)
			ret = -EINVAL;
	} else {
		ret = ptx_chrdev_context_search_minor(chrdev_ctx, num,
						      PTX_CHRDEV_MINOR_FREE,
						      &base);
		if (ret)
			dev_err(dev,
				"ptx_chrdev_context_add: no enough minor number%s.\n",
				(num == 1) ? "" : "s");
		else
			ptx_chrdev_context_set_minor_status(chrdev_ctx,
							    base, num,
							    PTX_CHRDEV_MINOR_RESERVED);
	}

	if (ret)
		goto fail;

	ptx_chrdev_context_set_minor_status(chrdev_ctx,
					    base, num,
					    PTX_CHRDEV_MINOR_IN_USE);

	group = kzalloc(sizeof(*group) + (sizeof(group->chrdev[0]) * num),
			GFP_KERNEL);
	if (!group) {
		ret = -ENOMEM;
		goto fail_group;
	}

	mutex_init(&group->lock);
	atomic_set(&group->available, 1);
	group->parent = chrdev_ctx;
	group->dev = dev;
	group->owner_kref = config->owner_kref;
	group->owner_kref_release = config->owner_kref_release;
	group->minor_base = MINOR(chrdev_ctx->dev_base) + base;
	group->chrdev_num = 0;

	for (i = 0; i < num; i++) {
		struct ptx_chrdev *chrdev = &group->chrdev[i];
		const struct ptx_chrdev_config *chrdev_config = &config->chrdev_config[i];

		mutex_init(&chrdev->lock);
		atomic_set(&chrdev->open, 0);
		chrdev->system_cap = chrdev_config->system_cap;
		chrdev->current_system = PTX_UNSPECIFIED_SYSTEM;
		chrdev->ops = chrdev_config->ops;
		chrdev->parent = group;
		memset(&chrdev->params, 0, sizeof(chrdev->params));
		chrdev->options = chrdev_config->options;
		chrdev->streaming = false;
		init_waitqueue_head(&chrdev->ringbuf_wait);
		chrdev->ringbuf_threshold_size = chrdev_config->ringbuf_threshold_size;
		chrdev->ringbuf_write_size = 0;
		chrdev->priv = chrdev_config->priv;

		ret = ringbuffer_create(&chrdev->ringbuf);
		if (ret) {
			mutex_destroy(&chrdev->lock);
			dev_err(dev,
				"ptx_chrdev_context_add: ringbuffer_create() failed. (ret: %d)\n",
				ret);
			break;
		}

		ret = ringbuffer_alloc(chrdev->ringbuf,
				       chrdev_config->ringbuf_size);
		if (ret) {
			ringbuffer_destroy(chrdev->ringbuf);
			mutex_destroy(&chrdev->lock);
			dev_err(dev,
				"ptx_chrdev_context_add: ringbuffer_alloc(%zu) failed. (ret: %d)\n",
				chrdev_config->ringbuf_size, ret);
			break;
		}

		if (chrdev->ops->init) {
			ret = chrdev->ops->init(chrdev);
			if (ret) {
				ringbuffer_destroy(chrdev->ringbuf);
				mutex_destroy(&chrdev->lock);
				dev_err(dev,
					"ptx_chrdev_context_add: chrdev->ops->init(%u) failed. (ret: %d)\n",
					i, ret);
				break;
			}
		}

		chrdev->id = i;
		group->chrdev_num++;
	}

	if (ret)
		goto fail_chrdev;

	cdev_init(&group->cdev, &ptx_chrdev_fops);
	group->cdev.owner = THIS_MODULE;

	ret = cdev_add(&group->cdev,
		       MKDEV(MAJOR(chrdev_ctx->dev_base), group->minor_base),
		       num);
	if (ret < 0) {
		dev_err(dev,
			"ptx_chrdev_context_add: cdev_add() failed. (ret: %d)\n",
			ret);
		goto fail_chrdev;
	}

	for (i = 0; i < num; i++) {
		dev_info(dev, "/dev/%s%u\n", chrdev_ctx->devname, base + i);
		if (strncmp(chrdev_ctx->devname, "isdb2056", 8) == 0) {
			// If the device is ISDB2056 or ISDB2056N, print the model name
			struct ptx_chrdev *chrdev = &group->chrdev[i];
			struct isdb2056_chrdev *chrdev2056 = chrdev->priv;
			struct isdb2056_device *isdb2056 = container_of(chrdev2056, struct isdb2056_device, chrdev2056);
			const char *model_name;
			switch (isdb2056->isdb2056_model) {
			case ISDB2056_MODEL:
				model_name = "ISDB2056";
				break;
			case ISDB2056N_MODEL:
				model_name = "ISDB2056N";
				break;
			default:
				model_name = "Unknown";
				break;
			}
			dev_info(dev, "/dev/%s%u: Digibest %s\n", chrdev_ctx->devname, base + i, model_name);
		}
		device_create(chrdev_ctx->class, dev,
			      MKDEV(MAJOR(chrdev_ctx->dev_base),
				    group->minor_base + i),
			      NULL, "%s%u", chrdev_ctx->devname, base + i);
	}

	kref_init(&group->kref);
	group->id = chrdev_ctx->last_id++;

	list_add_tail(&group->list, &chrdev_ctx->group_list);
	mutex_unlock(&chrdev_ctx->lock);

	if (chrdev_group)
		*chrdev_group = group;

	return 0;

fail_chrdev:
	if (group) {
		for (i = 0; i < group->chrdev_num; i++) {
			struct ptx_chrdev *chrdev = &group->chrdev[i];

			if (chrdev->ops->term)
				chrdev->ops->term(chrdev);

			ringbuffer_destroy(chrdev->ringbuf);
			mutex_destroy(&chrdev->lock);
		}

		mutex_destroy(&group->lock);
		kfree(group);
	}

fail_group:
	ptx_chrdev_context_set_minor_status(chrdev_ctx,
					    base, num,
					    (config->reserved) ? PTX_CHRDEV_MINOR_RESERVED : PTX_CHRDEV_MINOR_FREE);

fail:
	if (config->owner_kref)
		kref_put(config->owner_kref, config->owner_kref_release);

	mutex_unlock(&chrdev_ctx->lock);
	kref_put(&chrdev_ctx->kref, ptx_chrdev_context_release);

	return ret;
}

int ptx_chrdev_context_remove_group(struct ptx_chrdev_context *chrdev_ctx,
				    unsigned int minor_base)
{
	struct ptx_chrdev_group *group;

	mutex_lock(&chrdev_ctx->lock);
	if (!ptx_chrdev_context_search_group(chrdev_ctx,
					     minor_base, &group)) {
		/* not found */
		mutex_unlock(&chrdev_ctx->lock);
		return -ENOENT;
	}
	mutex_unlock(&chrdev_ctx->lock);

	ptx_chrdev_group_destroy(group);
	return 0;
}

static void ptx_chrdev_group_release(struct kref *kref)
{
	struct ptx_chrdev_group *group = container_of(kref,
						      struct ptx_chrdev_group,
						      kref);
	struct ptx_chrdev_context *ctx = group->parent;
	unsigned int i, minor_base, num;

	dev_dbg(group->dev, "ptx_chrdev_group_release\n");

	minor_base = group->minor_base;
	num = group->chrdev_num;

	for (i = 0; i < num; i++) {
		struct ptx_chrdev *chrdev = &group->chrdev[i];

		if (chrdev->ops->term)
			chrdev->ops->term(chrdev);

		ringbuffer_destroy(chrdev->ringbuf);
		mutex_destroy(&chrdev->lock);
	}

	mutex_destroy(&group->lock);
	kfree(group);

	mutex_lock(&ctx->lock);
	ptx_chrdev_context_set_minor_status(ctx,
					    minor_base - MINOR(ctx->dev_base),
					    num,
					    PTX_CHRDEV_MINOR_FREE);
	mutex_unlock(&ctx->lock);

	kref_put(&ctx->kref, ptx_chrdev_context_release);

	return;
}

void ptx_chrdev_group_destroy(struct ptx_chrdev_group *chrdev_group)
{
	struct ptx_chrdev_context *ctx = chrdev_group->parent;
	struct kref *owner_kref = chrdev_group->owner_kref;
	void (*owner_kref_release)(struct kref *) = chrdev_group->owner_kref_release;
	unsigned int i;

	dev_dbg(chrdev_group->dev,
		"ptx_chrdev_group_destroy: kref count: %u\n",
		kref_read(&chrdev_group->kref));

	mutex_lock(&ctx->lock);
	list_del(&chrdev_group->list);
	mutex_unlock(&ctx->lock);

	mutex_lock(&chrdev_group->lock);

	atomic_xchg(&chrdev_group->available, 0);

	for (i = 0; i < chrdev_group->chrdev_num; i++) {
		wake_up(&chrdev_group->chrdev[i].ringbuf_wait);
		device_destroy(ctx->class,
			       MKDEV(MAJOR(ctx->dev_base),
				     chrdev_group->minor_base + i));
	}

	cdev_del(&chrdev_group->cdev);

	mutex_unlock(&chrdev_group->lock);
	kref_put(&chrdev_group->kref, ptx_chrdev_group_release);

	if (owner_kref)
		kref_put(owner_kref, owner_kref_release);

	return;
}

int ptx_chrdev_put_stream(struct ptx_chrdev *chrdev, void *buf, size_t len)
{
	int ret = 0;

	ret = ringbuffer_write_atomic(chrdev->ringbuf, buf, &len);
	if (unlikely(ret && ret != -EOVERFLOW))
		return ret;

	chrdev->ringbuf_write_size += len;

	if (unlikely(chrdev->ringbuf_write_size >= chrdev->ringbuf_threshold_size)) {
		wake_up(&chrdev->ringbuf_wait);
		chrdev->ringbuf_write_size -= chrdev->ringbuf_threshold_size;
	}

	return ret;
}
