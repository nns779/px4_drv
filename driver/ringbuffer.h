// ringbuffer.h

#ifndef __RINGBUFFER_H__
#define __RINGBUFFER_H__

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

struct ringbuffer {
	spinlock_t lock;	// for data_size
	atomic_t empty;
	wait_queue_head_t wait;
	u8 *buf;
	size_t buf_size;
	size_t data_size;
	size_t tail_pos;	// write
	size_t head_pos;	// read
};

int ringbuffer_init(struct ringbuffer *ringbuffer, size_t size);
int ringbuffer_term(struct ringbuffer *ringbuffer);
int ringbuffer_flush(struct ringbuffer *ringbuffer);
int ringbuffer_write(struct ringbuffer *ringbuffer, const void *data, size_t len);
int ringbuffer_read_to_user(struct ringbuffer *ringbuffer, void __user *buf, size_t *len);

#endif
