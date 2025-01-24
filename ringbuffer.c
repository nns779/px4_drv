// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ringbuffer implementation (ringbuffer.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

#include "ringbuffer.h"

#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

static void ringbuffer_free_nolock(struct ringbuffer *ringbuf);
static void ringbuffer_lock(struct ringbuffer *ringbuf);

int ringbuffer_create(struct ringbuffer **ringbuf)
{
	struct ringbuffer *p;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	atomic_set(&p->state, 0);
	atomic_set(&p->rw_count, 0);
	atomic_set(&p->wait_count, 0);
	init_waitqueue_head(&p->wait);
	p->buf = NULL;
	p->size = 0;
	atomic_set(&p->actual_size, 0);
	atomic_set(&p->head, 0);
	atomic_set(&p->tail, 0);

	*ringbuf = p;

	return 0;
}

int ringbuffer_destroy(struct ringbuffer *ringbuf)
{
	ringbuffer_stop(ringbuf);

	ringbuffer_lock(ringbuf);
	ringbuffer_free_nolock(ringbuf);
	kfree(ringbuf);

	return 0;
}

static void ringbuffer_free_nolock(struct ringbuffer *ringbuf)
{
	if (ringbuf->buf)
		free_pages((unsigned long)ringbuf->buf,
			   get_order(ringbuf->size));

	ringbuf->buf = NULL;
	ringbuf->size = 0;

	return;
}

static void ringbuffer_reset_nolock(struct ringbuffer *ringbuf)
{
	atomic_set(&ringbuf->actual_size, 0);
	atomic_set(&ringbuf->head, 0);
	atomic_set(&ringbuf->tail, 0);

	return;
}

static void ringbuffer_lock(struct ringbuffer *ringbuf)
{
	atomic_add_return(1, &ringbuf->wait_count);
	wait_event(ringbuf->wait, !atomic_read(&ringbuf->rw_count));

	return;
}

static void ringbuffer_unlock(struct ringbuffer *ringbuf)
{
	if (atomic_sub_return(1, &ringbuf->wait_count))
		wake_up(&ringbuf->wait);

	return;
}

int ringbuffer_alloc(struct ringbuffer *ringbuf, size_t size)
{
	int ret = 0;

	if (size > INT_MAX)
		return -EINVAL;

	if (atomic_read_acquire(&ringbuf->state))
		return -EBUSY;

	ringbuffer_lock(ringbuf);

	if (ringbuf->buf && ringbuf->size != size)
		ringbuffer_free_nolock(ringbuf);

	ringbuf->size = 0;
	ringbuffer_reset_nolock(ringbuf);

	if (!ringbuf->buf) {
#ifdef __GFP_RETRY_MAYFAIL
		ringbuf->buf = (u8 *)__get_free_pages(GFP_KERNEL | __GFP_RETRY_MAYFAIL,
						      get_order(size));
#else
		ringbuf->buf = (u8 *)__get_free_pages(GFP_KERNEL | __GFP_REPEAT,
						      get_order(size));
#endif
		if (!ringbuf->buf)
			ret = -ENOMEM;
		else
			ringbuf->size = size;
	}

	ringbuffer_unlock(ringbuf);

	return ret;
}

int ringbuffer_free(struct ringbuffer *ringbuf)
{
	if (atomic_read_acquire(&ringbuf->state))
		return -EBUSY;

	ringbuffer_lock(ringbuf);
	ringbuffer_reset_nolock(ringbuf);
	ringbuffer_free_nolock(ringbuf);
	ringbuffer_unlock(ringbuf);

	return 0;
}

int ringbuffer_reset(struct ringbuffer *ringbuf)
{
	if (atomic_read_acquire(&ringbuf->state))
		return -EBUSY;

	ringbuffer_lock(ringbuf);
	ringbuffer_reset_nolock(ringbuf);
	ringbuffer_unlock(ringbuf);

	return 0;
}

int ringbuffer_start(struct ringbuffer *ringbuf)
{
	if (atomic_cmpxchg(&ringbuf->state, 0, 1))
		return -EALREADY;

	return 0;
}

int ringbuffer_stop(struct ringbuffer *ringbuf)
{
	if (!atomic_xchg(&ringbuf->state, 0))
		return -EALREADY;

	return 0;
}

int ringbuffer_ready_read(struct ringbuffer *ringbuf)
{
	if (!atomic_cmpxchg(&ringbuf->state, 1, 2))
		return -EINVAL;

	return 0;
}

int ringbuffer_read_user(struct ringbuffer *ringbuf,
			 void __user *buf, size_t *len)
{
	int ret = 0;
	u8 *p;
	size_t buf_size, actual_size, head, read_size;

	atomic_add_return_acquire(1, &ringbuf->rw_count);

	p = ringbuf->buf;
	buf_size = ringbuf->size;
	actual_size = atomic_read_acquire(&ringbuf->actual_size);
	head = atomic_read(&ringbuf->head);

	read_size = (*len <= actual_size) ? *len : actual_size;
	if (likely(read_size)) {
		unsigned long res;

		if (likely(head + read_size <= buf_size)) {
			res = copy_to_user(buf, p + head, read_size);
			if (unlikely(res)) {
				read_size -= res;
				ret = -EFAULT;
			}

			head = (head + read_size == buf_size) ? 0
							      : (head + read_size);
		} else {
			size_t tmp = buf_size - head;

			res = copy_to_user(buf, p + head, tmp);
			if (likely(!res))
				res = copy_to_user(((u8 *)buf) + tmp, p,
						   read_size - tmp);

			if (unlikely(res)) {
				read_size -= res;
				ret = -EFAULT;
			}

			head = read_size - tmp;
		}

		atomic_xchg(&ringbuf->head, head);
		atomic_sub_return_release(read_size,
					  &ringbuf->actual_size);
	}

	if (unlikely(!atomic_sub_return(1, &ringbuf->rw_count) &&
	    atomic_read(&ringbuf->wait_count)))
		wake_up(&ringbuf->wait);

	*len = read_size;

	return ret;
}

int ringbuffer_write_atomic(struct ringbuffer *ringbuf,
			    const void *buf, size_t *len)
{
	int ret = 0;
	u8 *p;
	size_t buf_size, actual_size, tail, write_size;

	if (unlikely(atomic_read(&ringbuf->state) != 2))
		return -EINVAL;

	atomic_add_return_acquire(1, &ringbuf->rw_count);

	p = ringbuf->buf;
	buf_size = ringbuf->size;
	actual_size = atomic_read_acquire(&ringbuf->actual_size);
	tail = atomic_read(&ringbuf->tail);

	write_size = likely(actual_size + *len <= buf_size) ? *len
							    : (buf_size - actual_size);
	if (likely(write_size)) {
		if (likely(tail + write_size <= buf_size)) {
			memcpy(p + tail, buf, write_size);
			tail = unlikely(tail + write_size == buf_size) ? 0
								       : (tail + write_size);
		} else {
			size_t tmp = buf_size - tail;

			memcpy(p + tail, buf, tmp);
			memcpy(p, ((u8 *)buf) + tmp, write_size - tmp);
			tail = write_size - tmp;
		}

		atomic_xchg(&ringbuf->tail, tail);
		atomic_add_return_release(write_size,
					  &ringbuf->actual_size);
	}

	if (unlikely(!atomic_sub_return(1, &ringbuf->rw_count) &&
	    atomic_read(&ringbuf->wait_count)))
		wake_up(&ringbuf->wait);

	if (unlikely(*len != write_size))
		ret = -EOVERFLOW;

	*len = write_size;

	return ret;
}

bool ringbuffer_is_running(struct ringbuffer *ringbuf)
{
	return !!atomic_read_acquire(&ringbuf->state);
}

bool ringbuffer_is_readable(struct ringbuffer *ringbuf)
{
	return !!atomic_read_acquire(&ringbuf->actual_size);
}
