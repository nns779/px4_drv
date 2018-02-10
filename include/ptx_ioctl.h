// ptx_ioctl.h

#ifndef __PTX_IOCTL_H__
#define __PTX_IOCTL_H__

struct ptx_freq {
	int freq_no;
	int slot;
};

#define PTX_SET_CHANNEL		_IOW(0x8d, 0x01, struct ptx_freq)
#define PTX_START_STREAMING	_IO(0x8d, 0x02)
#define PTX_STOP_STREAMING	_IO(0x8d, 0x03)
#define PTX_GET_CNR		_IOR(0x8d, 0x04, int *)

#endif
