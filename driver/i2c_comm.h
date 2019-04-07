// i2c_comm.h

#ifndef __I2C_COMM_H__
#define __I2C_COMM_H__

struct i2c_comm_master {
	int (*wr) (void *i2c_priv, u8 addr, const u8 * data, int len);
	int (*rd) (void *i2c_priv, u8 addr, u8 *data, int len);
	void *priv;
};

static inline int i2c_comm_master_write(struct i2c_comm_master *m, u8 addr, const u8 *data, int len)
{
	if (m && m->wr)
		return m->wr(m->priv, addr, data, len);
	else
		return -EFAULT;
}

static inline int i2c_comm_master_read(struct i2c_comm_master *m, u8 addr, u8 *data, int len)
{
	if (m && m->rd)
		return m->rd(m->priv, addr, data, len);
	else
		return -EFAULT;
}

#endif
