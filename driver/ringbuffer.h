// ringbuffer.h

#ifndef __RINGBUFFER_H__
#define __RINGBUFFER_H__

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

struct ringbuffer {
	spinlock_t lock;	// for data_size
	atomic_t avail;
	atomic_t rw_cnt;
	atomic_t wait_cnt;
	wait_queue_head_t wait;
	wait_queue_head_t data_wait;
	u8 *buf;
	size_t buf_size;
	size_t data_size;
	size_t tail_pos;	// write
	size_t head_pos;	// read
};

int ringbuffer_create(struct ringbuffer **ringbuffer);
int ringbuffer_destroy(struct ringbuffer *ringbuffer);
int ringbuffer_alloc(struct ringbuffer *ringbuffer, size_t size);
int ringbuffer_free(struct ringbuffer *ringbuffer);
int ringbuffer_start(struct ringbuffer *ringbuffer);
int ringbuffer_stop(struct ringbuffer *ringbuffer);
int ringbuffer_write_atomic(struct ringbuffer *ringbuffer, const void *data, size_t len);
int ringbuffer_read_user(struct ringbuffer *ringbuffer, void __user *buf, size_t *len);

#endif
