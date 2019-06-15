// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ringbuffer functions (ringbuffer.c)
 *
 * Copyright (c) 2018-2019 nns779
 */

#include "print_format.h"

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "ringbuffer.h"

static int _ringbuffer_init(struct ringbuffer *ringbuffer)
{
#ifdef RINGBUFFER_USE_SPINLOCK
	spin_lock_init(&ringbuffer->lock);
#endif
	atomic_set(&ringbuffer->avail, 0);
	atomic_set(&ringbuffer->rw_cnt, 0);
	atomic_set(&ringbuffer->wait_cnt, 0);
	init_waitqueue_head(&ringbuffer->wait);
	init_waitqueue_head(&ringbuffer->data_wait);
	ringbuffer->buf = NULL;
	ringbuffer->buf_size = 0;
#ifdef RINGBUFFER_USE_SPINLOCK
	ringbuffer->data_size = 0;
#else
	atomic_set(&ringbuffer->data_size, 0);
#endif
	ringbuffer->tail_pos = 0;
	ringbuffer->head_pos = 0;
	ringbuffer->write_size = 0;
	ringbuffer->write_threshold_size = 0;

	return 0;
}

int ringbuffer_create(struct ringbuffer **ringbuffer)
{
	int ret = 0;
	struct ringbuffer *p;

	*ringbuffer = NULL;

	p = kzalloc(sizeof(struct ringbuffer), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	ret = _ringbuffer_init(p);
	if (!ret)
		*ringbuffer = p;
	else
		kfree(p);

	return ret;
}

int ringbuffer_destroy(struct ringbuffer *ringbuffer)
{
	int ret = 0;

	ret = ringbuffer_free(ringbuffer);
	if (!ret)
		kfree(ringbuffer);

	return ret;
}

static void _ringbuffer_free(struct ringbuffer *ringbuffer)
{
	free_pages((unsigned long)ringbuffer->buf, get_order(ringbuffer->buf_size));

	ringbuffer->buf = NULL;
	ringbuffer->buf_size = 0;
#ifdef RINGBUFFER_USE_SPINLOCK
	ringbuffer->data_size = 0;
#else
	atomic_set(&ringbuffer->data_size, 0);
#endif
	ringbuffer->tail_pos = 0;
	ringbuffer->head_pos = 0;
	ringbuffer->write_size = 0;
	ringbuffer->write_threshold_size = 0;
}

int ringbuffer_alloc(struct ringbuffer *ringbuffer, size_t size)
{
	int ret = 0;

	// Acquire lock
	if (atomic_add_return(1, &ringbuffer->wait_cnt) != 1) {
		// Someone is waiting
		ret = -EAGAIN;
		goto exit;
	}
	atomic_set(&ringbuffer->avail, 0);
	wake_up(&ringbuffer->data_wait);
	wait_event(ringbuffer->wait, !atomic_read(&ringbuffer->rw_cnt));

	if (ringbuffer->buf) {
		if (ringbuffer->buf_size == size)
			goto reset;

		_ringbuffer_free(ringbuffer);
	}

	// Allocate

	ringbuffer->buf = (u8 *)__get_free_pages(GFP_KERNEL, get_order(size));
	if (!ringbuffer->buf) {
		ret = -ENOMEM;
		goto exit;
	}

	ringbuffer->buf_size = size;
	ringbuffer->write_threshold_size = (size / 10);

reset:
#ifdef RINGBUFFER_USE_SPINLOCK
	ringbuffer->data_size = 0;
#else
	atomic_set(&ringbuffer->data_size, 0);
#endif
	ringbuffer->tail_pos = 0;
	ringbuffer->head_pos = 0;
	ringbuffer->write_size = 0;

exit:
	// Release lock
	atomic_sub(1, &ringbuffer->wait_cnt);

	return ret;
}

int ringbuffer_free(struct ringbuffer *ringbuffer)
{
	int ret = 0;

	// Acquire lock
	if (atomic_add_return(1, &ringbuffer->wait_cnt) != 1) {
		// Someone is waiting
		ret = -EAGAIN;
		goto exit;
	}
	atomic_set(&ringbuffer->avail, 0);
	wake_up(&ringbuffer->data_wait);
	wait_event(ringbuffer->wait, !atomic_read(&ringbuffer->rw_cnt));

	if (!ringbuffer->buf)
		goto exit;

	_ringbuffer_free(ringbuffer);

exit:
	// Release lock
	atomic_sub(1, &ringbuffer->wait_cnt);

	return ret;
}

int ringbuffer_start(struct ringbuffer *ringbuffer)
{
	int ret = 0;

	if (atomic_read(&ringbuffer->avail))
		return 0;

	// Acquire lock for read buffer pointer
	if (atomic_add_return(1, &ringbuffer->wait_cnt) != 1) {
		// Someone is waiting
		ret = -EAGAIN;
		goto exit;
	}

	if (ringbuffer->buf && ringbuffer->buf_size)
		atomic_set(&ringbuffer->avail, 1);

exit:
	// Release lock
	atomic_sub(1, &ringbuffer->wait_cnt);

	return ret;
}

int ringbuffer_stop(struct ringbuffer *ringbuffer)
{
	atomic_set(&ringbuffer->avail, 0);
	wake_up(&ringbuffer->data_wait);

	return 0;
}

int ringbuffer_write_atomic(struct ringbuffer *ringbuffer, const void *data, size_t len)
{
#ifdef RINGBUFFER_USE_SPINLOCK
	unsigned long flags;
#endif
	const u8 *p = data;
	size_t buf_size, data_size, tail_pos, write_size;

	if (atomic_read(&ringbuffer->avail) != 2)
		return -EIO;

	atomic_add(1, &ringbuffer->rw_cnt);

	if (atomic_read(&ringbuffer->wait_cnt)) {
		atomic_sub(1, &ringbuffer->rw_cnt);
		wake_up(&ringbuffer->wait);
		return -EIO;
	}

	buf_size = ringbuffer->buf_size;
	tail_pos = ringbuffer->tail_pos;

#ifdef RINGBUFFER_USE_SPINLOCK
	spin_lock_irqsave(&ringbuffer->lock, flags);
	data_size = ringbuffer->data_size;
	spin_unlock_irqrestore(&ringbuffer->lock, flags);
#else
	data_size = atomic_read(&ringbuffer->data_size);
#endif

	write_size = (data_size + len <= buf_size) ? (len) : (buf_size - data_size);
	if (write_size) {
		size_t t = (tail_pos + write_size <= buf_size) ? (write_size) : (buf_size - tail_pos);

		memcpy(ringbuffer->buf + tail_pos, p, t);
		if (t < write_size) {
			memcpy(ringbuffer->buf, p + t, write_size - t);
			tail_pos = write_size - t;
		} else {
			tail_pos = (tail_pos + write_size == buf_size) ? 0 : (tail_pos + write_size);
		}

		ringbuffer->tail_pos = tail_pos;

#ifdef RINGBUFFER_USE_SPINLOCK
		spin_lock_irqsave(&ringbuffer->lock, flags);
		ringbuffer->data_size += write_size;
		spin_unlock_irqrestore(&ringbuffer->lock, flags);
#else
		atomic_add(write_size, &ringbuffer->data_size);
#endif

		if (ringbuffer->write_size >= ringbuffer->write_threshold_size) {
			if (atomic_read(&ringbuffer->rw_cnt) > 1) {
				wake_up(&ringbuffer->data_wait);
				ringbuffer->write_size = 0;
			}
		} else
			ringbuffer->write_size += write_size;
	}

	if (!atomic_sub_return(1, &ringbuffer->rw_cnt) && atomic_read(&ringbuffer->wait_cnt))
		wake_up(&ringbuffer->wait);

	return (write_size != len) ? (-ECANCELED) : (0);
}

int ringbuffer_read_user(struct ringbuffer *ringbuffer, void __user *buf, size_t *len)
{
	int ret = 0;
	u8 *p = buf;
	size_t buf_size, l = *len, buf_pos = 0;

	atomic_cmpxchg(&ringbuffer->avail, 1, 2);

	atomic_add(1, &ringbuffer->rw_cnt);

	if (atomic_read(&ringbuffer->wait_cnt)) {
		atomic_sub(1, &ringbuffer->rw_cnt);
		wake_up(&ringbuffer->wait);
		return -EIO;
	}

	buf_size = ringbuffer->buf_size;

	while (l > buf_pos) {
#ifdef RINGBUFFER_USE_SPINLOCK
		unsigned long flags;
#endif
		size_t data_size, head_pos, read_size, t;
		unsigned long r;

		if (wait_event_interruptible(ringbuffer->data_wait, (
#ifdef RINGBUFFER_USE_SPINLOCK
			(ringbuffer->data_size)
#else
			atomic_read(&ringbuffer->data_size)
#endif
			|| !atomic_read(&ringbuffer->avail)))
		) {
			if (!buf_pos)
				ret = -EINTR;
			break;
		}

#ifdef RINGBUFFER_USE_SPINLOCK
		spin_lock_irqsave(&ringbuffer->lock, flags);
		data_size = ringbuffer->data_size;
		spin_unlock_irqrestore(&ringbuffer->lock, flags);
#else
		data_size = atomic_read(&ringbuffer->data_size);
#endif

		if (!data_size) {
			if (!buf_pos)
				ret = -EIO;
			break;
		}

		head_pos = ringbuffer->head_pos;

		read_size = (l - buf_pos > data_size) ? (data_size) : (l - buf_pos);

		t = (head_pos + read_size <= buf_size) ? (read_size) : (buf_size - head_pos);

		r = copy_to_user(p + buf_pos, ringbuffer->buf + head_pos, t);
		if (r)
			pr_debug("ringbuffer_read_user: copy_to_user() 1 failed. remain: %lu\n", r);

		if (t < read_size) {
			r = copy_to_user(p + buf_pos + t, ringbuffer->buf, read_size - t);
			if (r)
				pr_debug("ringbuffer_read_user: copy_to_user() 2 failed. remain: %lu\n", r);

			head_pos = read_size - t;
		} else {
			head_pos = (head_pos + read_size == buf_size) ? 0 : (head_pos + read_size);
		}

		ringbuffer->head_pos = head_pos;
		buf_pos += read_size;

#ifdef RINGBUFFER_USE_SPINLOCK
		spin_lock_irqsave(&ringbuffer->lock, flags);
		ringbuffer->data_size -= read_size;
		spin_unlock_irqrestore(&ringbuffer->lock, flags);
#else
		atomic_sub(read_size, &ringbuffer->data_size);
#endif
	}

	*len = buf_pos;

	if (!atomic_sub_return(1, &ringbuffer->rw_cnt) && atomic_read(&ringbuffer->wait_cnt))
		wake_up(&ringbuffer->wait);

	return ret;
}
