// misc_win.h

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>

#include <windows.h>

#include "msg.h"

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define ARRAY_SIZE(arr)	(sizeof(arr) / sizeof((arr)[0]))

#define msleep(ms)		Sleep((ms) + (16 - ((ms) % 16)))
#define mdelay(ms)		/* do nothing*/

#define GFP_KERNEL	0
#define GFP_ATOMIC	0

#define kmalloc(size, gfp)	malloc(size)
#define kcalloc(n, size, gfp)	calloc(n, size)
#define kfree(p)		free(p)

static inline void * kzalloc(size_t size, int dummy)
{
	void *p;

	p = malloc(size);
	if (p)
		memset(p, 0, size);

	return p;
}

struct mutex {
	CRITICAL_SECTION s;
};

static inline void mutex_init(struct mutex *lock)
{
	InitializeCriticalSection(&lock->s);
}

static inline void mutex_destroy(struct mutex *lock)
{
	DeleteCriticalSection(&lock->s);
}

static inline void mutex_lock(struct mutex *lock)
{
	EnterCriticalSection(&lock->s);
}

static inline void mutex_unlock(struct mutex *lock)
{
	LeaveCriticalSection(&lock->s);
}

struct device {
	char driver_name[64];
	char device_name[64];
};

#define printk(format, ...)			msg_printf(format, ##__VA_ARGS__)

#define dev_print(level, dev, format, ...)	msg_printf("[" level "] %s %s: " format, (dev)->driver_name, (dev)->device_name, ##__VA_ARGS__)

#define dev_err(dev, format, ...)		dev_print("ERR", dev, format, ##__VA_ARGS__)
#define dev_warn(dev, format, ...)		dev_print("WARN", dev, format, ##__VA_ARGS__)
#define dev_info(dev, format, ...)		dev_print("INFO", dev, format, ##__VA_ARGS__)
#if defined(_DEBUG) || defined(_DEBUG_MSG)
#define dev_dbg(dev, format, ...)		dev_print("DBG", dev, format, ##__VA_ARGS__)
#else
#define dev_dbg(dev, format, ...)
#endif

typedef int atomic_t;

static inline void atomic_set(atomic_t *p, int v)
{
	_ReadWriteBarrier();
	*p = v;
	_ReadWriteBarrier();
}

static inline int atomic_read(const atomic_t *p)
{
	int v;

	_ReadWriteBarrier();
	v = *p;
	_ReadWriteBarrier();

	return v;
}

struct firmware {
	size_t size;
	const u8 *data;
};

int request_firmware(const struct firmware **fw, const char *name, struct device *);
void release_firmware(const struct firmware *fw);
