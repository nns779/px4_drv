// r850_channel.h

#ifndef __R850_CHANNEL_H__
#define __R850_CHANNEL_H__

#include "r850.h"

int r850_channel_get_regs(u32 no, u8 regs[2][R850_NUM_REGS - 0x08]);

#endif
