// ringbuffer.c

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uaccess.h>

#include "ringbuffer.h"

int ringbuffer_init(struct ringbuffer *ringbuffer, size_t size)
{
	ringbuffer_term(ringbuffer);

	ringbuffer->buf = (u8 *)__get_free_pages(GFP_KERNEL, get_order(size));
	if (!ringbuffer->buf) {
		return -ENOMEM;
	}
	ringbuffer->buf_size = size;
	ringbuffer->data_size = 0;

	spin_lock_init(&ringbuffer->lock);
	init_waitqueue_head(&ringbuffer->wait);
	atomic_set(&ringbuffer->empty, 0);

	ringbuffer->tail_pos = ringbuffer->head_pos = 0;

	return 0;
}

int ringbuffer_term(struct ringbuffer *ringbuffer)
{
	if (ringbuffer->buf) {
		unsigned long p = (unsigned long)ringbuffer->buf;
		ringbuffer->buf = NULL;
		free_pages(p, get_order(ringbuffer->buf_size));
	}

	return 0;
}

int ringbuffer_flush(struct ringbuffer *ringbuffer)
{
	ringbuffer->tail_pos = ringbuffer->head_pos = 0;
	ringbuffer->data_size = 0;
	atomic_set(&ringbuffer->empty, 1);
	wake_up(&ringbuffer->wait);

	return 0;
}

int ringbuffer_write(struct ringbuffer *ringbuffer, const void *data, size_t len)
{
	unsigned long flags;
	const u8 *p = data;
	size_t buf_size, data_size, tail_pos, write_size;

	if (!ringbuffer->buf)
		return -EINVAL;

	buf_size = ringbuffer->buf_size;
	tail_pos = ringbuffer->tail_pos;

	spin_lock_irqsave(&ringbuffer->lock, flags);
	data_size = ringbuffer->data_size;
	spin_unlock_irqrestore(&ringbuffer->lock, flags);

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

		spin_lock_irqsave(&ringbuffer->lock, flags);
		ringbuffer->data_size += write_size;
		wake_up(&ringbuffer->wait);
		spin_unlock_irqrestore(&ringbuffer->lock, flags);
	}

	return 0;
}

int ringbuffer_read_to_user(struct ringbuffer *ringbuffer, void __user *buf, size_t *len)
{
	u8 *p = buf;
	size_t buf_size, l = *len, buf_pos = 0;

	buf_size = ringbuffer->buf_size;

	while (l > buf_pos && !atomic_read(&ringbuffer->empty)) {
		size_t data_size, head_pos, read_size, t;
		unsigned long r;

		if (!ringbuffer->buf)
			break;

		wait_event(ringbuffer->wait, (ringbuffer->data_size || atomic_read(&ringbuffer->empty)));

		spin_lock(&ringbuffer->lock);
		data_size = ringbuffer->data_size;
		spin_unlock(&ringbuffer->lock);

		if (!data_size)
			break;

		head_pos = ringbuffer->head_pos;

		read_size = (l - buf_pos > data_size) ? (data_size) : (l - buf_pos);

		t = (head_pos + read_size <= buf_size) ? (read_size) : (buf_size - head_pos);

		r = copy_to_user(p + buf_pos, ringbuffer->buf + head_pos, t);
		if (r)
			pr_debug("ringbuffer_read_to_user: copy_to_user() 1 failed. remain: %lu\n", r);

		if (t < read_size) {
			r = copy_to_user(p + buf_pos + t, ringbuffer->buf, read_size - t);
			if (r)
				pr_debug("ringbuffer_read_to_user: copy_to_user() 2 failed. remain: %lu\n", r);

			head_pos = read_size - t;
		} else {
			head_pos = (head_pos + read_size == buf_size) ? 0 : (head_pos + read_size);
		}

		ringbuffer->head_pos = head_pos;
		buf_pos += read_size;

		spin_lock(&ringbuffer->lock);
		ringbuffer->data_size -= read_size;
		spin_unlock(&ringbuffer->lock);
	}

	*len = buf_pos;

	return 0;
}
