// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ringbuffer definitions (ringbuffer.h)
 *
 * Copyright (c) 2018-2021 nns779
 */

#ifndef __RINGBUFFER_H__
#define __RINGBUFFER_H__

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/wait.h>

struct ringbuffer {
	atomic_t state;
	atomic_t rw_count;
	atomic_t wait_count;
	wait_queue_head_t wait;
	u8 *buf;
	size_t size;
	atomic_t actual_size;
	atomic_t head;	// read
	atomic_t tail;	// write
};

int ringbuffer_create(struct ringbuffer **ringbuf);
int ringbuffer_destroy(struct ringbuffer *ringbuf);
int ringbuffer_alloc(struct ringbuffer *ringbuf, size_t size);
int ringbuffer_free(struct ringbuffer *ringbuf);
int ringbuffer_reset(struct ringbuffer *ringbuf);
int ringbuffer_start(struct ringbuffer *ringbuf);
int ringbuffer_stop(struct ringbuffer *ringbuf);
int ringbuffer_ready_read(struct ringbuffer *ringbuf);
int ringbuffer_read_user(struct ringbuffer *ringbuf,
			 void __user *buf, size_t *len);
int ringbuffer_write_atomic(struct ringbuffer *ringbuf,
			    const void *buf, size_t *len);
bool ringbuffer_is_readable(struct ringbuffer *ringbuf);
bool ringbuffer_is_running(struct ringbuffer *ringbuf);

#endif
