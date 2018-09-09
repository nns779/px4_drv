// it930x.c

// ITE IT930x driver

#include "print_format.h"

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/firmware.h>

#include "it930x-config.h"
#include "it930x-bus.h"
#include "it930x.h"

#define reg_addr_len(reg)	(((reg) & 0xff000000) ? 4 : (((reg) & 0x00ff0000) ? 3 : (((reg) & 0x0000ff00) ? 2 : 1)))

struct ctrl_buf {
	u8 *buf;
	u8 len;
};

static u16 calc_checksum(const void *buf, size_t len)
{
	int i;
	const u8 *b;
	u16 c = 0;

	i = len / 2;
	b = buf;

	while (i--) {
		c += ((b[0] << 8) | (b[1]));
		b += 2;
	}

	if (len % 2)
		c += (b[0] << 8);

	return ~c;
}

static int it930x_control(struct it930x_bridge *it930x, u16 cmd, struct ctrl_buf *buf, struct ctrl_buf *rbuf, u8 *rcode, bool no_rx)
{
	int ret;
	u8 *b, l, seq;
	u16 csum1, csum2;
	int rl = 255;

	if (!buf || buf->len > (255 - 4 - 2)) {
		pr_debug("it930x_control: Invalid parameter.\n");
		return -EINVAL;
	}

	b = it930x->buf;
	l = 3 + buf->len + 2;
	seq = it930x->sequence++;

	b[0] = l;
	b[1] = (cmd >> 8) & 0xff;
	b[2] = cmd & 0xff;
	b[3] = seq;
	if (buf->buf)
		memcpy(&b[4], buf->buf, buf->len);

	csum1 = calc_checksum(&b[1], l - 2);
	b[l - 1] = (csum1 >> 8) & 0xff;
	b[l] = csum1 & 0xff;

	ret = it930x_bus_ctrl_tx(&it930x->bus, b, l + 1, NULL);
	if (ret) {
		pr_debug("it930x_control: it930x_bus_ctrl_tx() failed. (cmd: %04x, len: %u, ret: %d)\n", cmd, buf->len, ret);
		return ret;
	}

	if (no_rx)
		return 0;

	ret = it930x_bus_ctrl_rx(&it930x->bus, b, &rl, NULL);
	if (ret) {
		pr_debug("it930x_control: it930x_bus_ctrl_rx() failed. (cmd: %04x, len: %u, rlen: %u, ret: %d)\n", cmd, buf->len, rl, ret);
		return ret;
	}

	if (rl < 5) {
		pr_debug("it930x_control: No enough response length. (cmd: %04x, len: %u, rlen: %u)\n", cmd, buf->len, rl);
		return -EBADMSG;
	}

	csum1 = calc_checksum(&b[1], rl - 3);
	csum2 = (((b[rl - 2] & 0xff) << 8) | (b[rl - 1] & 0xff));
	if (csum1 != csum2) {
		pr_debug("it930x_control: Incorrect checksum! (cmd: %04x, len: %u, rlen: %u, csum1: %04x, csum2: %04x)\n", cmd, buf->len, rl, csum1, csum2);
		return -EBADMSG;
	}

	if (b[1] != seq) {
		pr_debug("it930x_control: Incorrect sequence number! (cmd: %04x, len: %u, rlen: %u, seq: %02u, rseq: %02u, csum: %04x)\n", cmd, buf->len, rl, seq, b[1], csum1);
		return -EBADMSG;
	}

	if (b[2]) {
		pr_debug("it930x_control: Failed. (cmd: %04x, len: %u, rlen: %u, rcode: %u, csum: %04x)\n", cmd, buf->len, rl, b[2], csum1);
		ret = -EIO;
	} else if (rbuf) {
		if (rbuf->buf) {
			rbuf->len = ((rl - 3 - 2) > rbuf->len) ? rbuf->len : (rl - 3 - 2);
			memcpy(rbuf->buf, &b[3], rbuf->len);
		} else {
			rbuf->len = rl;
		}
	}

	if (rcode)
		*rcode = b[2];

	return ret;
}

int it930x_write_regs(struct it930x_bridge *it930x, struct it930x_regbuf *regbuf, int num)
{
	int ret = 0;
	int i;

	if (!regbuf || !num) {
		pr_debug("it930x_write_regs: Invaild parameter.\n");
		return -EINVAL;
	}

	for (i = 0; i < num; i++) {
		u8 b[249], len;
		u32 reg = regbuf[i].reg;
		struct ctrl_buf sb;

		if (regbuf[i].buf) {
			len = regbuf[i].u.len;

			if (!len || len > (249 - 6)) {
				pr_debug("it930x_write_regs: Buffer too large. (num: %d, i: %d, reg: %x)\n", num, i, reg);
				continue;
			}
			memcpy(&b[6], regbuf[i].buf, len);
		} else {
			len = 1;
			b[6] = regbuf[i].u.val;
		}

		b[0] = len;
		b[1] = reg_addr_len(reg);
		b[2] = (reg >> 24) & 0xff;
		b[3] = (reg >> 16) & 0xff;
		b[4] = (reg >> 8) & 0xff;
		b[5] = reg & 0xff;

		sb.buf = b;
		sb.len = 6 + len;

		ret = it930x_control(it930x, IT930X_CMD_REG_WRITE, &sb, NULL, NULL, false);
		if (ret) {
			pr_err("it930x_write_regs: it930x_control() failed. (num: %d, i: %d, reg: %x, len: %u)\n", num, i, reg, len);
			break;
		}
	}

	return ret;
}

int it930x_write_reg(struct it930x_bridge *it930x, u32 reg, u8 val)
{
	struct it930x_regbuf regbuf;

	regbuf.reg = reg;
	regbuf.buf = NULL;
	regbuf.u.val = val;

	return it930x_write_regs(it930x, &regbuf, 1);
}

int it930x_write_reg_bits(struct it930x_bridge *it930x, u32 reg, u8 val, u8 pos, u8 len)
{
	int ret = 0;
	u8 tmp;
	struct it930x_regbuf regbuf;

	if (len > 8) {
		pr_debug("it930x_write_reg_bits: Invalid parameter.\n");
		return -EINVAL;
	}

	regbuf.reg = reg;
	regbuf.buf = &tmp;
	regbuf.u.len = 1;

	if (len < 8) {
		ret = it930x_read_regs(it930x, &regbuf, 1);
		if (ret) {
			pr_err("it930x_write_reg_bits: it930x_read_regs() failed. (reg: %x, val: %u, pos: %u, len: %u, ret: %d)\n", reg, val, pos, len, ret);
			return ret;
		}

		tmp = (val << pos) | (tmp & (~((0xff) >> (8 - len) << pos)));
	} else {
		tmp = val;
	}

	ret = it930x_write_regs(it930x, &regbuf, 1);
	if (ret)
		pr_err("it930x_write_reg_bits: it930x_write_regs() failed. (reg: %x, val: %u, pos: %u, len: %u, t: %u, ret: %d)\n", reg, val, pos, len, tmp, ret);

	return ret;
}

int it930x_read_regs(struct it930x_bridge *it930x, struct it930x_regbuf *regbuf, int num)
{
	int ret = 0;
	int i;

	if (!regbuf || !num) {
		pr_debug("it930x_read_regs: Invald parameter.\n");
		return -EINVAL;
	}

	for (i = 0; i < num; i++) {
		u8 b[6];
		u32 reg = regbuf[i].reg;
		struct ctrl_buf sb, rb;

		if (!regbuf[i].buf || !regbuf[i].u.len) {
			pr_debug("it930x_read_regs: Invalid buffer. (num: %d, i: %d, reg: %x)\n", num, i, reg);
			continue;
		}

		b[0] = regbuf[i].u.len;
		b[1] = reg_addr_len(reg);
		b[2] = (reg >> 24) & 0xff;
		b[3] = (reg >> 16) & 0xff;
		b[4] = (reg >> 8) & 0xff;
		b[5] = reg & 0xff;

		sb.buf = b;
		sb.len = 6;

		rb.buf = regbuf[i].buf;
		rb.len = regbuf[i].u.len;

		ret = it930x_control(it930x, IT930X_CMD_REG_READ, &sb, &rb, NULL, false);
		if (ret) {
			pr_err("it930x_read_regs: it930x_control() failed. (num: %d, i: %d, reg: %x, len: %u, rlen: %u, ret: %d)\n", num, i, reg, regbuf[i].u.len, rb.len, ret);
			break;
		}

		if (rb.len != regbuf[i].u.len)
			pr_err("it930x_read_regs: Incorrect size! (num: %d, i: %d, reg: %x, len: %u, rlen: %u)\n", num, i, reg, regbuf[i].u.len, rb.len);
	}

	return ret;
}

int it930x_read_reg(struct it930x_bridge *it930x, u32 reg, u8 *val)
{
	struct it930x_regbuf regbuf = { reg, val, { 1 } };

	return it930x_read_regs(it930x, &regbuf, 1);
}

static int it930x_i2c_master_write(struct it930x_i2c_master_info *i2c, u8 addr, const u8 *data, int len)
{
	int ret = 0;
#ifdef IT930X_I2C_WRITE_REPEAT
	int ret2 = 0;
#endif
	u8 b[249];
	struct ctrl_buf sb;

	if (!data || !len) {
		pr_debug("it930x_i2c_master_write: Invalid parameter.\n");
		return -EINVAL;
	}

	if (len > (249 - 3)) {
		pr_debug("it930x_i2c_master_write: Buffer too large.\n");
		return -EINVAL;
	}

	b[0] = len;
	b[1] = i2c->bus;
	b[2] = addr;
	memcpy(&b[3], data, len);

	sb.buf = b;
	sb.len = 3 + len;

#ifdef IT930X_I2C_WRITE_REPEAT
	ret = it930x_write_reg(i2c->it930x, 0xf424, 1);
	if (ret) {
		pr_err("it930x_i2c_master_write: it930x_write_reg(0xf424, 1) failed. (ret: %d)\n", ret);
		return ret;
	}
#endif

	ret = it930x_control(i2c->it930x, IT930X_CMD_I2C_WRITE, &sb, NULL, NULL, false);

#ifdef IT930X_I2C_WRITE_REPEAT
	ret2 = it930x_write_reg(i2c->it930x, 0xf424, 0);
	if (ret2)
		pr_err("it930x_i2c_master_write: it930x_write_reg(0xf424, 0) failed. (ret: %d)\n", ret);

	return (ret) ? (ret) : (ret2);
#else
	return ret;
#endif
}

static int it930x_i2c_master_read(struct it930x_i2c_master_info *i2c, u8 addr, u8 *data, int len)
{
	u8 b[3];
	struct ctrl_buf sb, rb;

	if (!data || !len) {
		pr_debug("it930x_i2c_master_read: Invalid parameter.\n");
		return -EINVAL;
	}

	b[0] = len;
	b[1] = i2c->bus;
	b[2] = addr;

	sb.buf = b;
	sb.len = 3;

	rb.buf = data;
	rb.len = len;

	return it930x_control(i2c->it930x, IT930X_CMD_I2C_READ, &sb, &rb, NULL, false);
}

static int it930x_get_firmware_version(struct it930x_bridge *it930x)
{
	int ret = 0;
	u8 b[4];
	struct ctrl_buf sb, rb;

	b[0] = 1;

	sb.buf = b;
	sb.len = 1;

	rb.buf = b;
	rb.len = 4;

	ret = it930x_control(it930x, IT930X_CMD_QUERYINFO, &sb, &rb, NULL, false);
	if (!ret)
		it930x->fw_version = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];

	return ret;
}

static int it930x_enable_dvbt_mode(struct it930x_bridge *it930x, bool enable)
{
	int ret = 0;

	ret = it930x_write_reg_bits(it930x, 0xf41f, (enable) ? 1 : 0, 2, 1);
	if (ret)
		return ret;

	// mpeg full speed
	ret = it930x_write_reg_bits(it930x, 0xda10, (enable) ? 0 : 1, 0, 1);
	if (ret)
		return ret;

	// enable
	ret = it930x_write_reg_bits(it930x, 0xf41a, (enable) ? 1 : 0, 0, 1);
	if (ret)
		return ret;

	return 0;
}

static int it930x_enable_stream_output(struct it930x_bridge *it930x, bool enable, u32 xfer_size)
{
	int ret = 0, ret2 = 0;

	ret = it930x_write_reg_bits(it930x, 0xda1d, 1, 0, 1);
	if (ret)
		return ret;

	switch(it930x->bus.type) {
	case IT930X_BUS_USB:
		// disable ep
		ret = it930x_write_reg_bits(it930x, 0xdd11, 0, 5, 1);
		if (ret)
			goto end_rst_off;

		// nak
		ret = it930x_write_reg_bits(it930x, 0xdd13, (enable) ? 0 : 1, 5, 1);
		if (ret)
			goto end_rst_off;

		// enable ep
		ret = it930x_write_reg_bits(it930x, 0xdd11, 1, 5, 1);
		if (ret)
			goto end_rst_off;

		if (enable) {
			struct it930x_regbuf regbuf[2];
			u16 x;
			u8 b[2];

			x = (xfer_size / 4) & 0xffff;

			b[0] = (x & 0xff);
			b[1] = ((x >> 8) & 0xff);

			it930x_regbuf_set_buf(&regbuf[0], 0xdd88, b, 2);
			it930x_regbuf_set_val(&regbuf[1], 0xdd0c, 512 / 4/* USB2.0 */);
			ret = it930x_write_regs(it930x, regbuf, 2);
			if (ret)
				goto end_rst_off;

			ret = it930x_write_reg_bits(it930x, 0xda05, 0, 0, 1);
			if (ret)
				goto end_rst_off;

			ret = it930x_write_reg_bits(it930x, 0xda06, 0, 0, 1);
			if (ret)
				goto end_rst_off;
		}

		break;

	default:
		break;
	}

end_rst_off:
	ret2 = it930x_write_reg_bits(it930x, 0xda1d, 0, 0, 1);

	// reverse: no
	ret2 = it930x_write_reg(it930x, 0xd920, 0);

	return (ret) ? ret : ret2;
}
#if 0
static int it930x_set_xfer_size(struct it930x_bridge *it930x, u32 xfer_size)
{
	int ret = 0, ret2 = 0;
	struct it930x_regbuf regbuf[2];
	u16 x;
	u8 b[2];

	if (it930x->bus.type != IT930X_BUS_USB)
		// only for usb
		return 0;

	ret = it930x_write_reg_bits(it930x, 0xda1d, 1, 0, 1);
	if (ret)
		return ret;

	x = (xfer_size / 4) & 0xffff;

	b[0] = (x & 0xff);
	b[1] = ((x >> 8) & 0xff);

	it930x_regbuf_set_buf(&regbuf[0], 0xdd88, b, 2);
	it930x_regbuf_set_val(&regbuf[1], 0xdd0c, 512 / 4/* USB2.0 */);
	ret = it930x_write_regs(it930x, regbuf, 2);

	ret2 = it930x_write_reg_bits(it930x, 0xda1d, 0, 0, 1);

	return (ret) ? ret : ret2;
}
#endif
static int it930x_config_i2c(struct it930x_bridge *it930x)
{
	int ret = 0, i, j;
	u32 i2c_regs[5][2] = {
		{ 0x4975, 0x4971 },
		{ 0x4974, 0x4970 },
		{ 0x4973, 0x496f },
		{ 0x4972, 0x496e },
		{ 0x4964, 0x4963 }
	};
	struct it930x_regbuf regbuf[10];

	// set i2c speed

	it930x_regbuf_set_val(&regbuf[0], 0xf6a7, IT930X_I2C_SPEED);
	it930x_regbuf_set_val(&regbuf[1], 0xf103, IT930X_I2C_SPEED);
	ret = it930x_write_regs(it930x, regbuf, 2);
	if (ret)
		return ret;

	// set i2c address and bus

	for(i = 0, j = 0; i < 5; i++) {
		struct it930x_stream_input *input = &it930x->input[i];
		if (input->enable) {
			it930x_regbuf_set_val(&regbuf[j], i2c_regs[input->slave_number][0], input->i2c_addr);
			j++;
			it930x_regbuf_set_val(&regbuf[j], i2c_regs[input->slave_number][1], input->i2c_bus);
			j++;
		}
	}

	if (j) {
		ret = it930x_write_regs(it930x, regbuf, j);
		if (ret)
			return ret;
	}

	return 0;
}

static int it930x_config_stream_input(struct it930x_bridge *it930x)
{
	int ret = 0, i;

	for (i = 0; i < 5; i++) {
		struct it930x_stream_input *input = &it930x->input[i];
		struct it930x_regbuf regbuf[3];

		if (!input->enable) {
			// disable input port
			ret = it930x_write_reg(it930x, 0xda4c + input->port_number, 0);
			if (ret)
				break;

			continue;
		}

		if (input->port_number < 2) {
			ret = it930x_write_reg(it930x, 0xda58 + input->port_number, (input->is_parallel) ? 1 : 0);
			if (ret) {
				pr_err("it930x_config_stream_input: it930x_write_reg(0xda58 + port_number): failed. (idx: %d, ret: %d)\n", i, ret);
				break;
			}
		}

		// mode: sync byte
		it930x_regbuf_set_val(&regbuf[0], 0xda73 + input->port_number, 1);
		it930x_regbuf_set_val(&regbuf[1], 0xda78 + input->port_number, input->sync_byte);
		// enable input port
		it930x_regbuf_set_val(&regbuf[2], 0xda4c + input->port_number, 1);
		ret = it930x_write_regs(it930x, regbuf, 3);
		if (ret) {
			pr_err("it930x_config_stream_input: it930x_write_regs() failed. (idx: %d, ret: %d)\n", i, ret);
			break;
		}
	}

	return ret;
}

int it930x_init(struct it930x_bridge *it930x)
{
	int i;

	it930x->sequence = 0;

	// set i2c operator

	for (i = 0; i < 2; i++) {
		it930x->i2c[i].it930x = it930x;
		it930x->i2c[i].bus = i + 2;

		it930x->i2c_master[i].wr = (int (*)(void *, u8, const u8 *, int))it930x_i2c_master_write;
		it930x->i2c_master[i].rd = (int (*)(void *, u8, u8 *, int))it930x_i2c_master_read;
		it930x->i2c_master[i].priv = &it930x->i2c[i];
	}

	return 0;
}

int it930x_load_firmware(struct it930x_bridge *it930x, const char *filename)
{
	int ret = 0;
	const struct firmware *fw;
	size_t i, n, len = 0;
	struct ctrl_buf sb;

	if (!filename)
		return -EINVAL;

	ret = it930x_get_firmware_version(it930x);
	if (ret) {
		pr_err("it930x_load_firmware: it930x_get_firmware_version() failed. 1 (ret: %d)\n", ret);
		goto end;
	}

	if (it930x->fw_version)
		return 0;

	ret = it930x_write_reg(it930x, 0xf103, IT930X_I2C_SPEED);
	if (ret) {
		pr_err("it930x_load_firmware: it930x_write_reg(0xf103) failed. (ret: %d)\n", ret);
		return ret;
	}

	ret = request_firmware(&fw, filename, &it930x->bus.usb.dev->dev);
	if (ret) {
		pr_err("it930x_load_firmware: request_firmware() failed. (ret: %d)\n", ret);
		pr_err("Couldn't load firmware from the file.\n");
		return ret;
	}

	n = fw->size;

	for(i = 0; i < n; i += len) {
		const u8 *p = &fw->data[i];
		unsigned j, m = p[3];

		len = 0;

		if (p[0] != 0x03) {
			pr_err("it930x_load_firmware: Invalid firmware block was found. Abort. (ofs: %zx)\n", i);
			ret = -ECANCELED;
			goto end;
		}

		for(j = 0; j < m; j++)
			len += p[6 + (j * 3)];

		if (!len) {
			pr_warn("it930x_load_firmware: No data in the block. (ofs: %zx)\n", i);
			continue;
		}

		len += 4 + (m * 3);

		// send firmware block

		sb.buf = (u8 *)p;
		sb.len = len;

		ret = it930x_control(it930x, IT930X_CMD_FW_SCATTER_WRITE, &sb, NULL, NULL, false);
		if (ret) {
			pr_err("it930x_load_firmware: it930x_control(IT930X_CMD_FW_SCATTER_WRITE) failed. (ofs: %zx, ret: %d)\n", i, ret);
			goto end;
		}
	}

	sb.buf = NULL;
	sb.len = 0;

	ret = it930x_control(it930x, IT930X_CMD_BOOT, &sb, NULL, NULL, false);
	if (ret) {
		pr_err("it930x_load_firmware: it930x_control(IT930X_CMD_BOOT) failed. (ret: %d)\n", ret);
		goto end;
	}

	ret = it930x_get_firmware_version(it930x);
	if (ret) {
		pr_err("it930x_load_firmware: it930x_get_firmware_version() failed. 2 (ret: %d)\n", ret);
		goto end;
	}

	if (!it930x->fw_version) {
		ret = -EREMOTEIO;
		goto end;
	}

	pr_info("Firmware loaded. version: %x.%x.%x.%x\n", ((it930x->fw_version >> 24) & 0xff), ((it930x->fw_version >> 16) & 0xff), ((it930x->fw_version >> 8) & 0xff), (it930x->fw_version & 0xff));

end:
	release_firmware(fw);
	return ret;
}

int it930x_init_device(struct it930x_bridge *it930x)
{
	int ret = 0;
	struct it930x_regbuf regbuf[4];

	if (it930x->bus.type != IT930X_BUS_USB) {
		pr_debug("it930x_init_device: This driver only supports usb.\n");
		return -EINVAL;
	}

	it930x_regbuf_set_val(&regbuf[0], 0x4976, 0x00);
	it930x_regbuf_set_val(&regbuf[1], 0x4bfb, 0x00);
	it930x_regbuf_set_val(&regbuf[2], 0x4978, 0x00);
	it930x_regbuf_set_val(&regbuf[3], 0x4977, 0x00);
	ret = it930x_write_regs(it930x, regbuf, 4);
	if (ret)
		return ret;

	// ignore sync byte: no
	ret = it930x_write_reg(it930x, 0xda1a, 0);
	if (ret)
		return ret;

	ret = it930x_enable_dvbt_mode(it930x, true);
	if (ret) {
		pr_err("it930x_init_device: it930x_enable_dvbt_mode() failed.\n");
		return ret;
	}

	ret = it930x_enable_stream_output(it930x, true, it930x->bus.usb.streaming_xfer_size);
	if (ret) {
		pr_err("it930x_init_device: it930x_enable_stream_output() failed.\n");
		return ret;
	}

	// power config
	it930x_regbuf_set_val(&regbuf[0], 0xd833, 1);
	it930x_regbuf_set_val(&regbuf[1], 0xd830, 0);
	it930x_regbuf_set_val(&regbuf[2], 0xd831, 1);
	it930x_regbuf_set_val(&regbuf[3], 0xd832, 0);
	ret = it930x_write_regs(it930x, regbuf, 4);
	if (ret)
		return ret;

	ret = it930x_config_i2c(it930x);
	if (ret) {
		pr_err("it930x_init_device: it930x_config_i2c() failed. (ret: %d)\n", ret);
		return ret;
	}

	ret = it930x_config_stream_input(it930x);
	if (ret) {
		pr_err("it930x_init_device: it930x_config_stream_input() failed. (ret: %d)\n", ret);
		return ret;
	}

	return 0;
}

int it930x_set_gpio(struct it930x_bridge *it930x, int gpio, bool h)
{
	u32 gpio_en_regs[] = {
		0xd8b0,		// gpioh1
		0xd8b8,		// gpioh2
		0xd8b4,		// gpioh3
		0xd8c0,		// gpioh4
		0xd8bc,		// gpioh5
		0xd8c8,		// gpioh6
		0xd8c4,		// gpioh7
		0xd8d0,		// gpioh8
		0xd8cc,		// gpioh9
		0xd8d8,		// gpioh10
		0xd8d4,		// gpioh11
		0xd8e0,		// gpioh12
		0xd8dc,		// gpioh13
		0xd8e4,		// gpioh14
		0xd8e8,		// gpioh15
		0xd8ec,		// gpioh16
	};
	struct it930x_regbuf regbuf[3];

	if (gpio <= 0 || gpio > (sizeof(gpio_en_regs) / sizeof(gpio_en_regs[0])))
		return -EINVAL;

	it930x_regbuf_set_val(&regbuf[0], gpio_en_regs[gpio - 1], 1);
	it930x_regbuf_set_val(&regbuf[1], gpio_en_regs[gpio - 1] + 0x01, 1);
	it930x_regbuf_set_val(&regbuf[2], gpio_en_regs[gpio - 1] - 0x01, (h) ? 1 : 0);

	return it930x_write_regs(it930x, regbuf, 3);
}

int it930x_enable_stream_input(struct it930x_bridge *it930x, u8 input_idx, bool enable)
{
	if (input_idx >= 5)
		return -EINVAL;

	return it930x_write_reg(it930x, 0xda4c + it930x->input[input_idx].port_number, (enable) ? 1 : 0);
}

int it930x_purge_psb(struct it930x_bridge *it930x)
{
	int ret = 0;
	void *p;
	int len;

	if (it930x->bus.type != IT930X_BUS_USB)
		return 0;

	ret = it930x_write_reg_bits(it930x, 0xda1d, 1, 0, 1);
	if (ret)
		return ret;

	len = it930x->bus.usb.streaming_xfer_size;

	p = kmalloc(len, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	ret = it930x_bus_stream_rx(&it930x->bus, p, &len, 2000);
	pr_debug("it930x_purge_psb: len: %d\n", len);

	kfree(p);

	it930x_write_reg_bits(it930x, 0xda1d, 0, 0, 1);

	return ret;
}
