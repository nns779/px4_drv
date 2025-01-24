// SPDX-License-Identifier: GPL-2.0-only
/*
 * RafaelMicro R850 driver (r850.c)
 *
 * Copyright (c) 2018-2021 nns779
 */

/* Some features are not implemented. */

#include "print_format.h"
#include "r850.h"

#ifdef __linux__
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/delay.h>
#endif

enum r850_imr_direction {
	R850_IMR_DIRECTION_GAIN = 0,
	R850_IMR_DIRECTION_PHASE = 1,
};

enum r850_calibration {
	R850_CALIBRATION_NONE = 0,
	R850_CALIBRATION_IMR,
	R850_CALIBRATION_LPF,
};

static const u8 init_regs[R850_NUM_REGS] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xca, 0xc0, 0x72, 0x50, 0x00, 0xe0, 0x00, 0x30,
	0x86, 0xbb, 0xf8, 0xb0, 0xd2, 0x81, 0xcd, 0x46,
	0x37, 0x40, 0x89, 0x8c, 0x55, 0x95, 0x07, 0x23,
	0x21, 0xf1, 0x4c, 0x5f, 0xc4, 0x20, 0xa9, 0x6c,
	0x53, 0xab, 0x5b, 0x46, 0xb3, 0x93, 0x6e, 0x41
};

static const u8 sleep_regs[R850_NUM_REGS] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x33, 0xee, 0xb9, 0xfe, 0x0f, 0xe1, 0x04, 0x30,
	0x86, 0xfb, 0xf8, 0xb0, 0xd2, 0x81, 0xcd, 0x46,
	0x37, 0x44, 0x89, 0x8c, 0x55, 0x95, 0x07, 0x23,
	0x21, 0xf1, 0x4c, 0x5f, 0xc4, 0x20, 0xa9, 0xfc,
	0x53, 0xab, 0x0b, 0x46, 0xb3, 0x93, 0x6e, 0x41
};

static const u8 wakeup_regs[R850_NUM_REGS] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xca, 0xe0, 0xf2, 0x7c, 0xc0, 0xe0, 0x00, 0x30,
	0x86, 0xbb, 0xf8, 0xb0, 0xd2, 0x81, 0xcd, 0x46,
	0x37, 0x40, 0x89, 0x8c, 0x55, 0x95, 0x07, 0x23,
	0x21, 0xf1, 0x4c, 0x5f, 0xc4, 0x20, 0xa9, 0x6c,
	0x53, 0xab, 0x5b, 0x46, 0xb3, 0x93, 0x6e, 0x41
};

static const u8 imr_cal_regs[R850_NUM_REGS] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xc0, 0x49, 0x3a, 0x90, 0x03, 0xc1, 0x61, 0x71,
	0x17, 0xf1, 0x18, 0x55, 0x30, 0x20, 0xf3, 0xed,
	0x1f, 0x1c, 0x81, 0x13, 0x00, 0x80, 0x0a, 0x07,
	0x21, 0x71, 0x54, 0xf1, 0xf2, 0xa9, 0xbb, 0x0b,
	0xa3, 0xf6, 0x0b, 0x44, 0x92, 0x17, 0xe6, 0x80
};

static const u8 lpf_cal_regs[R850_NUM_REGS] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xc0, 0x49, 0x3f, 0x90, 0x13, 0xe1, 0x89, 0x7a,
	0x07, 0xf1, 0x9a, 0x50, 0x30, 0x20, 0xe1, 0x00,
	0x00, 0x04, 0x81, 0x11, 0xef, 0xee, 0x17, 0x07,
	0x31, 0x71, 0x54, 0xb2, 0xee, 0xa9, 0xbb, 0x0b,
	0xa3, 0x00, 0x0b, 0x44, 0x92, 0x1f, 0xe6, 0x80
};

struct r850_lpf_params {
	u8 code;
	u8 bandwidth;
	u8 lsb;
};

struct r850_system_params {
	enum r850_bandwidth bandwidth;
	u32 if_freq;
	u32 filt_cal_if;
	u8 bw;
	u8 filt_ext_ena;
	u8 hpf_notch;
	u8 hpf_cor;
	u8 filt_comp;
	u8 img_gain;
	u8 agc_clk;
	struct r850_lpf_params lpf;
};

static const struct r850_system_params dvb_t_t2_params[2][6] = {
	{
		{ R850_BANDWIDTH_6M, 4570, 7550, 1, 0, 0, 0x08, 1, 0, 0, { 0x01, 3, 1 } },
		{ R850_BANDWIDTH_7M, 4570, 7920, 1, 0, 0, 0x0b, 1, 0, 0, { 0x04, 2, 0 } },
		{ R850_BANDWIDTH_8M, 4570, 8450, 0, 0, 0, 0x0c, 1, 0, 0, { 0x01, 2, 0 } },
		{ R850_BANDWIDTH_6M, 5000, 7920, 1, 0, 0, 0x06, 1, 0, 0, { 0x06, 2, 1 } },
		{ R850_BANDWIDTH_7M, 5000, 8450, 0, 0, 0, 0x09, 1, 0, 0, { 0x00, 2, 1 } },
		{ R850_BANDWIDTH_8M, 5000, 8700, 0, 0, 0, 0x0a, 1, 0, 0, { 0x06, 0, 1 } },
	},
	{
		{ R850_BANDWIDTH_6M, 4570, 7550, 1, 0, 0, 0x08, 1, 3, 1, { 0x01, 3, 1 } },
		{ R850_BANDWIDTH_7M, 4570, 7920, 1, 0, 0, 0x0b, 1, 3, 1, { 0x04, 2, 0 } },
		{ R850_BANDWIDTH_8M, 4570, 8450, 0, 0, 0, 0x0c, 1, 3, 1, { 0x01, 2, 0 } },
		{ R850_BANDWIDTH_6M, 5000, 7920, 1, 0, 0, 0x06, 1, 3, 1, { 0x06, 2, 1 } },
		{ R850_BANDWIDTH_7M, 5000, 8450, 0, 0, 0, 0x09, 1, 3, 1, { 0x00, 2, 1 } },
		{ R850_BANDWIDTH_8M, 5000, 8700, 0, 0, 0, 0x0a, 1, 3, 1, { 0x06, 0, 1 } },
	}
};

static const struct r850_system_params dvb_t2_1_params[2][2] = {
	{
		{ R850_BANDWIDTH_7M, 1900, 7920, 1, 0, 0, 0x08, 1, 0, 0, { 0x04, 2, 0 } },
		{ R850_BANDWIDTH_7M, 5000, 6000, 2, 0, 0, 0x01, 1, 0, 0, { 0x0b, 3, 1 } },
	},
	{
		{ R850_BANDWIDTH_7M, 1900, 7920, 1, 0, 0, 0x08, 1, 3, 1, { 0x04, 2, 0 } },
		{ R850_BANDWIDTH_7M, 5000, 6000, 2, 0, 0, 0x01, 1, 3, 1, { 0x0b, 3, 1 } },
	}
};

static const struct r850_system_params dvb_c_params[2][4] = {
	{
		{ R850_BANDWIDTH_6M, 5070, 8100, 1, 0, 0, 0x05, 1, 0, 0, { 0x02, 2, 0 } },
		{ R850_BANDWIDTH_8M, 5070, 9550, 0, 0, 0, 0x0b, 1, 0, 0, { 0x04, 0, 0 } },
		{ R850_BANDWIDTH_6M, 5000, 7780, 1, 0, 0, 0x06, 1, 0, 0, { 0x01, 2, 1 } },
		{ R850_BANDWIDTH_8M, 5000, 9250, 0, 0, 0, 0x0b, 1, 0, 0, { 0x05, 0, 1 } },
	},
	{
		{ R850_BANDWIDTH_6M, 5070, 8100, 1, 0, 0, 0x05, 1, 3, 1, { 0x02, 2, 0 } },
		{ R850_BANDWIDTH_8M, 5070, 9550, 0, 0, 0, 0x0b, 1, 3, 1, { 0x04, 0, 0 } },
		{ R850_BANDWIDTH_6M, 5000, 7780, 1, 0, 0, 0x06, 1, 3, 1, { 0x01, 2, 1 } },
		{ R850_BANDWIDTH_8M, 5000, 9250, 0, 0, 0, 0x0b, 1, 3, 1, { 0x05, 0, 1 } },
	}
};

static const struct r850_system_params j83b_params[2][2] = {
	{
		{ R850_BANDWIDTH_6M, 5070, 8100, 1, 0, 0, 0x05, 1, 0, 0, { 0x03, 2, 1 } },
		{ R850_BANDWIDTH_6M, 5000, 7550, 1, 0, 0, 0x05, 1, 0, 0, { 0x05, 2, 1 } },
	},
	{
		{ R850_BANDWIDTH_6M, 5070, 8100, 1, 0, 0, 0x05, 1, 3, 1, { 0x03, 2, 1 } },
		{ R850_BANDWIDTH_6M, 5000, 7550, 1, 0, 0, 0x05, 1, 3, 1, { 0x05, 2, 1 } },
	}
};

static const struct r850_system_params isdb_t_params[2][3] = {
	{
		{ R850_BANDWIDTH_6M, 4063, 7070, 1, 0, 0, 0x08, 1, 0, 0, { 0x02, 3, 1 } },
		{ R850_BANDWIDTH_6M, 4570, 7400, 1, 0, 0, 0x05, 1, 0, 0, { 0x08, 2, 0 } },
		{ R850_BANDWIDTH_6M, 5000, 7780, 1, 1, 0, 0x03, 1, 0, 0, { 0x05, 2, 0 } },
	},
	{
		{ R850_BANDWIDTH_6M, 4063, 7070, 1, 0, 0, 0x0a, 1, 3, 1, { 0x02, 3, 1 } },
		{ R850_BANDWIDTH_6M, 4570, 7400, 1, 0, 0, 0x08, 1, 3, 1, { 0x08, 2, 0 } },
		{ R850_BANDWIDTH_6M, 5000, 7780, 1, 0, 0, 0x03, 1, 3, 1, { 0x05, 2, 0 } },
	}
};

static const struct r850_system_params dtmb_params[2][4] = {
	{
		{ R850_BANDWIDTH_6M, 4500, 7200, 1, 0, 0, 0x08, 1, 0, 0, { 0x02, 3, 1 } },
		{ R850_BANDWIDTH_8M, 4570, 8450, 0, 0, 0, 0x0c, 1, 0, 0, { 0x00, 2, 1 } },
		{ R850_BANDWIDTH_6M, 5000, 8100, 1, 0, 0, 0x06, 1, 0, 0, { 0x04, 2, 1 } },
		{ R850_BANDWIDTH_8M, 5000, 8800, 0, 0, 0, 0x0b, 2, 0, 0, { 0x05, 0, 1 } },
	},
	{
		{ R850_BANDWIDTH_6M, 4500, 7200, 1, 0, 0, 0x08, 1, 3, 1, { 0x02, 3, 1 } },
		{ R850_BANDWIDTH_8M, 4570, 8450, 0, 0, 0, 0x0c, 1, 3, 1, { 0x00, 2, 1 } },
		{ R850_BANDWIDTH_6M, 5000, 8100, 1, 0, 0, 0x06, 1, 3, 1, { 0x04, 2, 1 } },
		{ R850_BANDWIDTH_8M, 5000, 8800, 0, 0, 0, 0x0b, 2, 3, 1, { 0x05, 0, 1 } },
	}
};

static const struct r850_system_params atsc_params[2][2] = {
	{
		{ R850_BANDWIDTH_6M, 5070, 8050, 1, 0, 0, 0x05, 1, 0, 0, { 0x03, 2, 0 } },
		{ R850_BANDWIDTH_6M, 5000, 7920, 1, 0, 0, 0x05, 1, 0, 0, { 0x04, 2, 0 } },
	},
	{
		{ R850_BANDWIDTH_6M, 5070, 8050, 1, 0, 0, 0x05, 1, 3, 1, { 0x03, 2, 0 } },
		{ R850_BANDWIDTH_6M, 5000, 7920, 1, 0, 0, 0x05, 1, 3, 1, { 0x04, 2, 0 } },
	}
};

static const struct r850_system_params *sys_params[10][2] = {
	{ NULL, NULL },
	{ dvb_t_t2_params[0], dvb_t_t2_params[1] },
	{ dvb_t_t2_params[0], dvb_t_t2_params[1] },
	{ dvb_t2_1_params[0], dvb_t2_1_params[1] },
	{ dvb_c_params[0], dvb_c_params[1] },
	{ j83b_params[0], j83b_params[1] },
	{ isdb_t_params[0], isdb_t_params[1] },
	{ dtmb_params[0], dtmb_params[1] },
	{ atsc_params[0], atsc_params[1] },
	{ NULL, NULL },
};

static const int sys_param_num[10][2] = {
	{ 0, 0 },
	{ ARRAY_SIZE(dvb_t_t2_params[0]), ARRAY_SIZE(dvb_t_t2_params[1]) },
	{ ARRAY_SIZE(dvb_t_t2_params[0]), ARRAY_SIZE(dvb_t_t2_params[1]) },
	{ ARRAY_SIZE(dvb_t2_1_params[0]), ARRAY_SIZE(dvb_t2_1_params[1]) },
	{ ARRAY_SIZE(dvb_c_params[0]), ARRAY_SIZE(dvb_c_params[1]) },
	{ ARRAY_SIZE(j83b_params[0]), ARRAY_SIZE(j83b_params[1]) },
	{ ARRAY_SIZE(isdb_t_params[0]), ARRAY_SIZE(isdb_t_params[1]) },
	{ ARRAY_SIZE(dtmb_params[0]), ARRAY_SIZE(dtmb_params[1]) },
	{ ARRAY_SIZE(atsc_params[0]), ARRAY_SIZE(atsc_params[1]) },
	{ 0, 0 },
};

struct r850_system_frequency_params {
	u32 if_freq;
	u32 rf_freq_min;
	u32 rf_freq_max;
	u8 lna_top;
	u8 lna_vtl_h;
	u8 lna_nrb_det;
	u8 lna_rf_dis_mode;
	u8 lna_rf_charge_cur;
	u8 lna_rf_dis_curr;
	u8 lna_dis_slow_fast;
	u8 rf_top;
	u8 rf_vtl_h;
	u8 rf_gain_limit;
	u8 rf_dis_slow_fast;
	u8 rf_lte_psg;
	u8 nrb_top;
	u8 nrb_bw_hpf;
	u8 nrb_bw_lpf;
	u8 mixer_top;
	u8 mixer_vth;
	u8 mixer_vtl;
	u8 mixer_amp_lpf;
	u8 mixer_gain_limit;
	u8 mixer_detbw_lpf;
	u8 mixer_filter_dis;
	u8 filter_top;
	u8 filter_vth;
	u8 filter_vtl;
	u8 filt_3th_lpf_cur;
	u8 filt_3th_lpf_gain;
	u8 bb_dis_curr;
	u8 bb_det_mode;
	u8 na_pwr_det;
	u8 enb_poly_gain;
	u8 img_nrb_adder;
	u8 hpf_comp;
	u8 fb_res_1st;
};

static const struct r850_system_frequency_params dvb_t_t2_freq_params[4] = {
	{
		0, 0, 340000,
		5, 0x5a, 0, 1, 1, 1, 0x05,
		4, 0x5a, 0, 0x05, 1,
		5, 0, 2,
		9, 0x09, 0x04, 4, 3, 0, 2,
		4, 0x09, 0x04,
		1, 3, 0, 0, 1, 0, 2, 1, 1
	},
	{
		0, 662001, 670000,
		4, 0x5a, 0, 4, 1, 1, 0x05,
		4, 0x5a, 0, 0x05, 1,
		4, 0, 2,
		9, 0x09, 0x04, 4, 3, 0, 2,
		4, 0x09, 0x04,
		1, 3, 0, 0, 1, 0, 2, 1, 1
	},
	{
		0, 782001, 790000,
		5, 0x5a, 0, 2, 0, 1, 0x05,
		4, 0x5a, 0, 0x05, 1,
		4, 0, 2,
		9, 0x09, 0x04, 4, 3, 0, 2,
		4, 0x09, 0x04,
		1, 3, 0, 0, 1, 0, 2, 1, 1
	},
	{
		0, 0, 0,
		4, 0x5a, 0, 1, 1, 1, 0x05,
		4, 0x5a, 0, 0x05, 1,
		4, 0, 2,
		9, 0x09, 0x04, 4, 3, 0, 2,
		4, 0x09, 0x04,
		1, 3, 0, 0, 1, 0, 2, 1, 1
	}
};

static const struct r850_system_frequency_params dvb_c_freq_params[2] = {
	{
		0, 0, 660000,
		4, 0x5a, 0, 1, 1, 1, 0x05,
		4, 0x4a, 0, 0x05, 0,
		5, 0, 2,
		12, 0x09, 0x04, 4, 2, 0, 0,
		12, 0x09, 0x04,
		1, 0, 1, 0, 1, 1, 2, 1, 1
	},
	{
		0, 0, 0,
		4, 0x5a, 0, 1, 1, 1, 0x05,
		3, 0x4a, 0, 0x05, 0,
		5, 0, 2,
		12, 0x09, 0x04, 4, 2, 0, 0,
		12, 0x09, 0x04,
		1, 0, 1, 0, 1, 1, 1, 1, 1
	}
};

static const struct r850_system_frequency_params j83b_freq_params[3] = {
	{
		0, 0, 335000,
		5, 0x5a, 0, 1, 1, 1, 0x05,
		4, 0x4a, 0, 0x05, 0,
		5, 0, 0,
		12, 0x09, 0x04, 7, 2, 0, 0,
		12, 0x09, 0x04,
		1, 0, 1, 0, 1, 1, 2, 1, 1
	},
	{
		0, 340001, 660000,
		5, 0x5a, 0, 1, 1, 1, 0x05,
		4, 0x4a, 0, 0x05, 0,
		5, 0, 0,
		12, 0x09, 0x04, 7, 2, 0, 0,
		12, 0x09, 0x04,
		1, 0, 1, 0, 1, 1, 2, 1, 1
	},
	{
		0, 0, 0,
		4, 0x5a, 0, 1, 1, 1, 0x05,
		3, 0x4a, 0, 0x05, 0,
		5, 0, 0,
		12, 0x09, 0x04, 7, 2, 0, 0,
		12, 0x09, 0x04,
		1, 0, 1, 0, 1, 1, 1, 1, 1
	}
};

static const struct r850_system_frequency_params isdb_t_freq_params[10] = {
	/* ISDB-T 4063 */
	{
		4063, 0, 340000,
		5, 0x6b, 0, 1, 1, 1, 0x05,
		5, 0x4a, 0, 0x05, 1,
		12, 0, 2,
		15, 0x09, 0x04, 7, 3, 0, 0,
		12, 0x09, 0x04,
		1, 0, 1, 0, 1, 0, 2, 2, 1
	},
	{
		4063, 470000, 487999,
		6, 0x8c, 0, 1, 1, 1, 0x05,
		5, 0x6b, 0, 0x05, 1,
		3, 0, 2,
		14, 0x09, 0x04, 7, 3, 0, 0,
		12, 0x09, 0x04,
		1, 3, 1, 0, 1, 1, 3, 2, 1
	},
	{
		4063, 680000, 691999,
		5, 0x5a, 0, 2, 1, 1, 0x07,
		6, 0x6b, 0, 0x04, 1,
		3, 0, 2,
		14, 0x09, 0x05, 7, 3, 0, 0,
		12, 0x09, 0x04,
		1, 3, 1, 0, 0, 1, 3, 2, 1
	},
	{
		4063, 692000, 697999,
		5, 0x5b, 0, 2, 1, 1, 0x07,
		6, 0x6b, 0, 0x04, 1,
		10, 0, 3,
		12, 0x09, 0x05, 7, 3, 0, 0,
		12, 0x09, 0x04,
		1, 3, 1, 0, 0, 1, 2, 2, 1
	},
	{
		4063, 0, 0,
		5, 0x5a, 0, 1, 1, 1, 0x05,
		6, 0x6b, 0, 0x05, 1,
		3, 0, 2,
		14, 0x09, 0x04, 7, 3, 0, 0,
		12, 0x09, 0x04,
		1, 3, 1, 0, 1, 1, 3, 2, 1
	},
	/* ISDB-T other */
	{
		0, 0, 340000,
		5, 0x6b, 0, 1, 1, 1, 0x05,
		5, 0x4a, 0, 0x05, 1,
		12, 0, 2,
		15, 0x0b, 0x06, 7, 3, 0, 0,
		12, 0x09, 0x04,
		1, 0, 1, 0, 1, 0, 2, 2, 1
	},
	{
		0, 470000, 487999,
		5, 0x5a, 0, 2, 1, 1, 0x07,
		6, 0x6b, 0, 0x04, 1,
		3, 0, 2,
		14, 0x09, 0x05, 7, 3, 0, 0,
		12, 0x09, 0x04,
		1, 3, 1, 0, 0, 1, 3, 2, 1
	},
	{
		0, 680000, 691999,
		5, 0x5b, 0, 2, 1, 1, 0x07,
		6, 0x6b, 0, 0x04, 1,
		10, 0, 3,
		12, 0x09, 0x05, 7, 3, 0, 0,
		12, 0x09, 0x04,
		1, 3, 1, 0, 0, 1, 2, 2, 1
	},
	{
		0, 692000, 697999,
		5, 0x5a, 0, 1, 1, 1, 0x05,
		6, 0x6b, 0, 0x05, 1,
		3, 0, 2,
		14, 0x09, 0x04, 7, 3, 0, 0,
		12, 0x09, 0x04,
		1, 3, 1, 0, 1, 1, 3, 2, 1
	},
	{
		0, 0, 0,
		5, 0x5a, 0, 1, 1, 1, 0x05,
		6, 0x6b, 0, 0x05, 1,
		3, 0, 2,
		14, 0x09, 0x04, 7, 3, 0, 0,
		12, 0x09, 0x04,
		1, 3, 1, 0, 1, 1, 3, 2, 1
	}
};

static const struct r850_system_frequency_params dtmb_freq_params[3] = {
	{
		0, 0, 100000,
		4, 0x6b, 0, 1, 1, 1, 0x05,
		4, 0x4a, 0, 0x05, 1,
		10, 3, 3,
		9, 0x09, 0x04, 4, 1, 0, 2,
		4, 0x09, 0x04,
		0, 0, 0, 0, 1, 0, 1, 0, 0
	},
	{
		0, 0, 340000,
		4, 0x6b, 0, 1, 1, 1, 0x05,
		4, 0x4a, 0, 0x05, 1,
		10, 0, 2,
		9, 0x09, 0x04, 4, 1, 0, 2,
		4, 0x09, 0x04,
		0, 0, 0, 0, 1, 0, 1, 0, 0
	},
	{
		0, 0, 0,
		4, 0x5a, 0, 1, 1, 1, 0x05,
		4, 0x4a, 0, 0x05, 1,
		6, 3, 2,
		9, 0x09, 0x04, 4, 1, 0, 2,
		4, 0x09, 0x04,
		0, 3, 0, 0, 1, 0, 0, 0, 0
	}
};

static const struct r850_system_frequency_params atsc_freq_params[2] = {
	{
		0, 0, 340000,
		6, 0x5a, 0, 1, 1, 1, 0x05,
		5, 0x6b, 0, 0x05, 1,
		12, 2, 2,
		12, 0x0b, 0x04, 7, 2, 1, 2,
		6, 0x09, 0x04,
		1, 0, 0, 0, 1, 0, 1, 2, 1
	},
	{
		0, 0, 0,
		6, 0x5a, 0, 1, 1, 1, 0x05,
		5, 0x6b, 0, 0x05, 1,
		12, 2, 2,
		12, 0x0b, 0x04, 7, 2, 1, 2,
		6, 0x09, 0x04,
		1, 3, 0, 0, 1, 0, 1, 2, 1
	}
};

static const struct r850_system_frequency_params *sys_freq_params[10] = {
	NULL,
	dvb_t_t2_freq_params,
	dvb_t_t2_freq_params,
	dvb_t_t2_freq_params,
	dvb_c_freq_params,
	j83b_freq_params,
	isdb_t_freq_params,
	dtmb_freq_params,
	atsc_freq_params,
	NULL,
};

static const int sys_freq_param_num[10] = {
	0,
	ARRAY_SIZE(dvb_t_t2_freq_params),
	ARRAY_SIZE(dvb_t_t2_freq_params),
	ARRAY_SIZE(dvb_t_t2_freq_params),
	ARRAY_SIZE(dvb_c_freq_params),
	ARRAY_SIZE(j83b_freq_params),
	ARRAY_SIZE(isdb_t_freq_params),
	ARRAY_SIZE(dtmb_freq_params),
	ARRAY_SIZE(atsc_freq_params),
	0,
};

static const u16 lna_acc_gain[32] = {
	0, 15, 26, 34, 50, 61, 75, 87,
	101, 117, 130, 144, 154, 164, 176, 188,
	199, 209, 220, 226, 233, 232, 232, 232,
	232, 247, 262, 280, 296, 311, 296, 308
};

static const u16 rf_acc_gain[16] = {
	0, 15, 26, 34, 50, 61, 75, 87,
	101, 117, 130, 144, 154, 164, 176, 188
};

static const u16 mixer_acc_gain[16] = {
	0, 0, 0, 0, 9, 22, 32, 44,
	56, 68, 80, 90, 100, 100, 100, 100
};

static u8 reverse_bit(u8 val)
{
	u8 t = val;

	t = (t & 0x55) << 1 | (t & 0xaa) >> 1;
	t = (t & 0x33) << 2 | (t & 0xcc) >> 2;
	t = (t & 0x0f) << 4 | (t & 0xf0) >> 4;

	return t;
}

static int r850_read_regs(struct r850_tuner *t, u8 reg, u8 *buf, int len)
{
	int ret = 0, i;
	u8 b[1 + R850_NUM_REGS];
	struct i2c_comm_request req[2];

	if (!t || !buf || !len)
		return -EINVAL;

	if (len > (R850_NUM_REGS - reg))
		return -EINVAL;

	b[0] = 0x00;

	req[0].req = I2C_WRITE_REQUEST;
	req[0].addr = t->i2c_addr;
	req[0].data = b;
	req[0].len = 1;

	req[1].req = I2C_READ_REQUEST;
	req[1].addr = t->i2c_addr;
	req[1].data = b;
	req[1].len = reg + len;

	ret = i2c_comm_master_request(t->i2c, req, 2);
	if (ret) {
		dev_err(t->dev,
			"r850_read_regs: i2c_comm_master_request() failed. (reg: 0x%02x, len: %d, ret: %d)\n",
			reg, len, ret);
	} else {
		for (i = reg; i < (reg + len); i++)
			buf[i - reg] = reverse_bit(b[i]);
	}

	return ret;
}

static int r850_write_regs(struct r850_tuner *t,
			   u8 reg,
			   const u8 *buf, int len)
{
	int ret = 0;
	u8 b[1 + R850_NUM_REGS];
	struct i2c_comm_request req[1];

	if (!t || !buf || !len)
		return -EINVAL;

	if (len > (R850_NUM_REGS - reg))
		return -EINVAL;

	b[0] = reg;
	memcpy(&b[1], buf, len);

	req[0].req = I2C_WRITE_REQUEST;
	req[0].addr = t->i2c_addr;
	req[0].data = b;
	req[0].len = 1 + len;

	ret = i2c_comm_master_request(t->i2c, req, 1);
	if (ret)
		dev_err(t->dev,
			"r850_write_regs: i2c_comm_master_request() failed. (reg: 0x%02x, len: %d, ret: %d)\n",
			reg, len, ret);

	return ret;
}

static int r850_init_regs(struct r850_tuner *t)
{
	memcpy(t->priv.regs, init_regs, sizeof(t->priv.regs));

#if 0
	if (t->config.loop_through) {
		t->priv.regs[0x08] |= 0xc0;
		t->priv.regs[0x0a] |= 0x02;
	} else {
		t->priv.regs[0x08] &= 0x3f;
		t->priv.regs[0x08] |= 0x40;
		t->priv.regs[0x0a] &= 0xfd;
	}

	if (t->config.clock_out)
		t->priv.regs[0x22] &= 0xfb;
	else
		t->priv.regs[0x22] |= 0x04;
#endif

	return 0;
}

static int r850_set_xtal_cap(struct r850_tuner *t, u8 cap)
{
	u8 c = cap;
	bool g = false;

	if (c > 0x1f) {
		c -= 10;
		g = true;
	}

	t->priv.regs[0x21] = (t->priv.regs[0x21] & 0x07) |
			     ((c << 2) & 0x78) | ((g) ? 0x80 : 0x00);
	t->priv.regs[0x22] = (t->priv.regs[0x22] & 0xf7) | ((c << 3) & 0x08);

	return 0;
}

static int r850_set_pll(struct r850_tuner *t,
			u32 lo_freq, u32 if_freq,
			enum r850_system sys)
{
	int ret = 0;
	u32 xtal, vco_min, vco_max, vco_freq;
	u16 nint, vco_fra;
	u16 nsdm = 2, sdm = 0;
	u8 mix_div = 2, div = 0, ni, si;
	u8 xtal_div;
	u8 b;
	u16 div_judge;

	xtal = t->config.xtal;

	vco_min = 2200000;
	if (!t->priv.chip)
		vco_min += 70000;

	vco_max = vco_min * 2;
	vco_freq = lo_freq * mix_div;

	t->priv.regs[0x20] &= 0xfc;
	t->priv.regs[0x2e] |= 0x40;
	t->priv.regs[0x0c] &= 0x3c;
	t->priv.regs[0x09] &= 0xf9;
	t->priv.regs[0x22] &= 0x3f;
	t->priv.regs[0x0b] &= 0xc3;
	t->priv.regs[0x0b] |= 0x10;
	t->priv.regs[0x25] &= 0xef;
	t->priv.regs[0x25] |= 0x20;

	if (lo_freq < 100000) {
		if (t->priv.xtal_pwr > 1)
			b = 3 - t->priv.xtal_pwr;
		else
			b = 2;
	} else if (lo_freq < 130000) {
		if (t->priv.xtal_pwr > 2)
			b = 3 - t->priv.xtal_pwr;
		else
			b = 1;
	} else {
		b = 0;
	}

	ret = r850_set_xtal_cap(t, 0x27);
	if (ret)
		return ret;

	t->priv.regs[0x22] &= 0xcf;
	t->priv.regs[0x22] |= ((b << 4) & 0x30);

	/* xtal == 24000 */
	div_judge = ((lo_freq + if_freq) / 1000 / 12);

	t->priv.regs[0x1e] &= 0x1f;
	t->priv.regs[0x25] &= 0xfd;

	switch (div_judge) {
	case 4:
	case 10:
	case 22:
	case 24:
	case 28:
		t->priv.regs[0x25] |= 0x02;
		break;

	default:
		t->priv.regs[0x25] |= 0x00;
		break;
	}

	if (t->priv.chip)
		t->priv.regs[0x2f] &= 0xfd;
	else
		t->priv.regs[0x2f] &= 0xfc;

	while (div < 6) {
		if (vco_min <= vco_freq && vco_freq < vco_max)
			break;

		mix_div *= 2;
		vco_freq = lo_freq * mix_div;

		div++;
	}

	xtal_div = 0;

	t->priv.regs[0x22] &= 0xfc;
	if (sys != R850_SYSTEM_UNDEFINED) {
		if (lo_freq < 380500) {
			if (!(div_judge & 1)) {
				xtal /= 2;
				t->priv.regs[0x22] |= 0x02;
				xtal_div = 1;
			}
		} else if ((lo_freq + if_freq - 478000) < 4000 &&
			   sys == R850_SYSTEM_ISDB_T) {
#if 1
			xtal /= 4;
			t->priv.regs[0x22] |= 0x03;
			xtal_div = 3;
#endif
		}
	}

	t->priv.regs[0x0b] &= 0xfe;

	t->priv.regs[0x2d] &= 0xf3;
	if (mix_div == 8)
		t->priv.regs[0x2d] |= 0x04;
	else if (mix_div == 16)
		t->priv.regs[0x2d] |= 0x08;
	else if (mix_div >= 32)
		t->priv.regs[0x2d] |= 0x0c;

	t->priv.regs[0x2e] &= 0xfc;
	t->priv.regs[0x20] &= 0xec;
	if (mix_div == 2 || mix_div == 4) {
		t->priv.regs[0x2e] |= 0x01;
	} else {
		t->priv.regs[0x2e] |= 0x02;
		t->priv.regs[0x20] |= 0x01;
	}

	t->priv.regs[0x11] &= 0x7f;
	if (mix_div == 8)
		t->priv.regs[0x11] |= 0x80;

	t->priv.regs[0x1e] &= 0xe3;
	t->priv.regs[0x1e] |= ((div << 2) & 0x1c);

	nint = (vco_freq / 2) / xtal;
	vco_fra = vco_freq - (xtal * 2 * nint);

	if (vco_fra < (xtal / 64)) {
		vco_fra = 0;
	} else if (vco_fra > (xtal * 127 / 64)) {
		vco_fra = 0;
		nint++;
	} else if (vco_fra > (xtal * 127 / 128) && (xtal > vco_fra)) {
		vco_fra = xtal * 127 / 128;
	} else if ((xtal < vco_fra) && (vco_fra < (xtal * 129 / 128))) {
		vco_fra = xtal * 129 / 128;
	}

	ni = (nint - 13) / 4;
	si = nint - 13 - (ni * 4);

	t->priv.regs[0x1b] &= 0x80;
	t->priv.regs[0x1b] |= (ni & 0x7f);

	t->priv.regs[0x1e] &= 0xfc;
	t->priv.regs[0x1e] |= (si & 0x03);

	t->priv.regs[0x20] &= 0x3f;

	while (vco_fra > 1) {
		if ((xtal * 2 / nsdm) < vco_fra) {
			vco_fra -= (xtal * 2) / nsdm;
			sdm += 0x8000 / (nsdm / 2);

			if (nsdm & 0x8000)
				break;
		}
		nsdm += nsdm;
	}

	t->priv.regs[0x1c] = (sdm & 0xff);
	t->priv.regs[0x1d] = ((sdm >> 8) & 0xff);

	ret = r850_write_regs(t, 0x08, &t->priv.regs[0x08], 0x28);
	if (ret)
		return ret;

	switch (xtal_div) {
	case 0:
		msleep(10);
		break;
	case 1:
	case 2:
		msleep(20);
		break;
	default:
		msleep(40);
		break;
	}

	if (!t->priv.chip)
		t->priv.regs[0x2f] &= 0xfc;

	t->priv.regs[0x2f] |= 0x02;

	return r850_write_regs(t, 0x2f, &t->priv.regs[0x2f], 0x01);
}

static int r850_set_mux(struct r850_tuner *t,
			u32 rf_freq, u32 lo_freq,
			enum r850_system sys)
{
	u8 imr_idx;
	u8 imr_gain, imr_phase, imr_iqcap;
	u8 rf_poly;
	u8 lpf_cap, lpf_notch;
	u8 tf_hpf_bpf, tf_hpf_cnr, tf_diplexer;

	if (lo_freq < 170000)
		imr_idx = 0;
	else if (lo_freq < 240000)
		imr_idx = 4;
	else if (lo_freq < 400000)
		imr_idx = 1;
	else if (lo_freq < 760000)
		imr_idx = 2;
	else
		imr_idx = 3;

	if (lo_freq < 580000)
		tf_hpf_bpf = 7;
	else if (lo_freq < 660000)
		tf_hpf_bpf = 1;
	else if (lo_freq < 780000)
		tf_hpf_bpf = 6;
	else if (lo_freq < 900000)
		tf_hpf_bpf = 4;
	else
		tf_hpf_bpf = 0;

	if (lo_freq < 133000)
		rf_poly = 2;
	else if (lo_freq < 221000)
		rf_poly = 1;
	else if (lo_freq < 760000)
		rf_poly = 0;
	else
		rf_poly = 3;

	if (lo_freq < 480000)
		tf_hpf_cnr = 3;
	else if (lo_freq < 550000)
		tf_hpf_cnr = 2;
	else if (lo_freq < 700000)
		tf_hpf_cnr = 1;
	else
		tf_hpf_cnr = 0;

	if (sys == R850_SYSTEM_DVB_C || sys == R850_SYSTEM_J83B) {
		if (lo_freq < 77000) {
			lpf_notch = 10;
			lpf_cap = 15;
		} else if (lo_freq < 85000) {
			lpf_notch = 4;
			lpf_cap = 15;
		} else if (lo_freq < 115000) {
			lpf_notch = 3;
			lpf_cap = 13;
		} else if (lo_freq < 125000) {
			lpf_notch = 1;
			lpf_cap = 11;
		} else if (lo_freq < 141000) {
			lpf_notch = 0;
			lpf_cap = 9;
		} else if (lo_freq < 157000) {
			lpf_notch = 0;
			lpf_cap = 8;
		} else if (lo_freq < 181000) {
			lpf_notch = 0;
			lpf_cap = 6;
		} else if (lo_freq < 205000) {
			lpf_notch = 0;
			lpf_cap = 3;
		} else {
			lpf_notch = 0;
			lpf_cap = 0;
		}
	} else {
		if (lo_freq < 73000) {
			lpf_notch = 10;
			lpf_cap = 8;
		} else if (lo_freq < 81000) {
			lpf_notch = 4;
			lpf_cap = 8;
		} else if (lo_freq < 89000) {
			lpf_notch = 3;
			lpf_cap = 8;
		} else if (lo_freq < 121000) {
			lpf_notch = 1;
			lpf_cap = 6;
		} else if (lo_freq < 145000) {
			lpf_notch = 0;
			lpf_cap = 4;
		} else if (lo_freq < 153000) {
			lpf_notch = 0;
			lpf_cap = 3;
		} else if (lo_freq < 177000) {
			lpf_notch = 0;
			lpf_cap = 2;
		} else if (lo_freq < 201000) {
			lpf_notch = 0;
			lpf_cap = 1;
		} else {
			lpf_notch = 0;
			lpf_cap = 0;
		}
	}

	tf_diplexer = (lo_freq < 330000) ? 2 : 0;

	if (t->priv.imr_cal[t->priv.mixer_mode].done &&
	    t->priv.imr_cal[t->priv.mixer_mode].result[imr_idx]) {
		struct r850_imr *imr = &t->priv.imr_cal[t->priv.mixer_mode].imr[imr_idx];

		imr_gain = imr->gain;
		imr_phase = imr->phase;
		imr_iqcap = imr->iqcap;
	} else if (sys != R850_SYSTEM_UNDEFINED) {
		imr_gain = 0x02;
		imr_phase = 0x00;
		imr_iqcap = 0x00;
	} else {
		imr_gain = 0x00;
		imr_phase = 0x00;
		imr_iqcap = 0x00;
	}

	t->priv.regs[0x0e] &= 0x03;
	t->priv.regs[0x0e] |= ((tf_diplexer << 2) & 0x0c);
	t->priv.regs[0x0e] |= ((lpf_cap << 4) & 0xf0);

	t->priv.regs[0x0f] &= 0xf0;
	t->priv.regs[0x0f] |= (lpf_notch & 0x0f);

	t->priv.regs[0x10] &= 0xe0;
	t->priv.regs[0x10] |= ((tf_hpf_cnr << 3) & 0x18);
	t->priv.regs[0x10] |= (tf_hpf_bpf & 0x07);

	t->priv.regs[0x12] &= 0xfc;
	t->priv.regs[0x12] |= (rf_poly & 0x03);

	t->priv.regs[0x14] &= 0xd0;
	t->priv.regs[0x14] |= (imr_gain & 0x2f);

	t->priv.regs[0x15] &= 0x10;
	t->priv.regs[0x15] |= (imr_phase & 0x2f);
	t->priv.regs[0x15] |= ((imr_iqcap << 6) & 0xc0);

	return 0;
}

static int r850_read_adc_value(struct r850_tuner *t, u8 *value)
{
	int ret = 0;
	u8 tmp;

	mdelay(2);

	ret = r850_read_regs(t, 0x01, &tmp, 1);
	if (!ret)
		*value = (tmp & 0x3f);

	return ret;
}

static int r850_imr_check_iq_cross(struct r850_tuner *t,
				   struct r850_imr *imr,
				   enum r850_imr_direction *direction)
{
	int ret = 0, i;
	struct r850_imr imr_tmp;
	struct {
		u8 gain;
		u8 phase;
	} cross[9] = { { 0 } };

	cross[1].phase = 1;
	cross[2].phase = (0x20 | 1);
	cross[3].gain = 1;
	cross[4].gain = (0x20 | 1);
	cross[5].phase = 2;
	cross[6].phase = (0x20 | 2);
	cross[7].gain = 2;
	cross[8].gain = (0x20 | 2);

	imr_tmp.value = 0xff;

	for (i = 0; i < 9; i++) {
		u8 tmp;

		t->priv.regs[0x14] &= 0xd0;
		t->priv.regs[0x14] |= (cross[i].gain & 0x2f);

		t->priv.regs[0x15] &= 0xd0;
		t->priv.regs[0x15] |= (cross[i].phase & 0x2f);

		ret = r850_write_regs(t, 0x14, &t->priv.regs[0x14], 2);
		if (ret)
			break;

		ret = r850_read_adc_value(t, &tmp);
		if (ret)
			break;

		if (imr_tmp.value > tmp) {
			imr_tmp.gain = cross[i].gain;
			imr_tmp.phase = cross[i].phase;
			imr_tmp.value = tmp;
		}
	}

	if (!ret) {
		*imr = imr_tmp;
		*direction = (imr_tmp.phase) ? R850_IMR_DIRECTION_PHASE
					     : R850_IMR_DIRECTION_GAIN;
	}

	return ret;
}

static int r850_imr_check_iq_tree(struct r850_tuner *t,
				  struct r850_imr *imr,
				  enum r850_imr_direction direction, int num)
{
	int ret = 0, i;
	struct r850_imr imr_tmp;
	u8 reg, val[5];

	if (num != 3 && num != 5)
		return -EINVAL;

	switch (direction) {
	case R850_IMR_DIRECTION_GAIN:
		reg = 0x14;
		val[0] = imr->gain;
		t->priv.regs[0x15] &= 0xd0;
		t->priv.regs[0x15] |= (imr->phase & 0x2f);
		imr_tmp.phase = imr->phase;
		break;

	case R850_IMR_DIRECTION_PHASE:
		reg = 0x15;
		val[0] = imr->phase;
		t->priv.regs[0x14] &= 0xd0;
		t->priv.regs[0x14] |= (imr->gain & 0x2f);
		imr_tmp.gain = imr->gain;
		break;

	default:
		return -EINVAL;
	}

	val[1] = val[0] + 1;

	if (num == 3) {
		if (!(val[0] & 0x0f))
			val[2] = ((val[0] ^ 0x20) + 1);
		else
			val[2] = val[0] - 1;
	} else if (num == 5) {
		val[2] = val[0] + 2;

		switch (val[0] & 0x0f) {
		case 0:
			val[3] = ((val[0] ^ 0x20) + 1);
			val[4] = val[3] + 1;
			break;

		case 1:
			val[3] = val[0] - 1;
			val[4] = ((val[3] ^ 0x20) + 1);
			break;

		default:
			val[3] = val[0] - 1;
			val[4] = val[3] - 1;
			break;
		}
	}

	imr_tmp.value = 0xff;

	for (i = 0; i < num; i++) {
		u8 tmp;

		t->priv.regs[reg] &= 0xd0;
		t->priv.regs[reg] |= (val[i] & 0x2f);

		ret = r850_write_regs(t, 0x14, &t->priv.regs[0x14], 2);
		if (ret)
			break;

		ret = r850_read_adc_value(t, &tmp);
		if (ret)
			break;

		if (imr_tmp.value > tmp) {
			if (direction == R850_IMR_DIRECTION_GAIN)
				imr_tmp.gain = val[i];
			else
				imr_tmp.phase = val[i];

			imr_tmp.value = tmp;
		}
	}

	if (!ret)
		*imr = imr_tmp;

	return ret;
}

static int r850_imr_check_iq_step(struct r850_tuner *t,
				  struct r850_imr *imr,
				  enum r850_imr_direction direction)
{
	int ret = 0;
	struct r850_imr imr_tmp;
	u8 reg, val;

	switch (direction) {
	case R850_IMR_DIRECTION_GAIN:
		reg = 0x14;
		val = imr->gain;
		t->priv.regs[0x15] &= 0xd0;
		t->priv.regs[0x15] |= (imr->phase & 0x2f);
		imr_tmp.phase = imr->phase;
		break;

	case R850_IMR_DIRECTION_PHASE:
		reg = 0x15;
		val = imr->phase;
		t->priv.regs[0x14] &= 0xd0;
		t->priv.regs[0x14] |= (imr->gain & 0x2f);
		imr_tmp.gain = imr->gain;
		break;

	default:
		return -EINVAL;
	}

	imr_tmp.gain = imr->gain;
	imr_tmp.phase = imr->phase;
	imr_tmp.value = imr->value;

	while ((val & 0x0f) <= 8) {
		u8 tmp;

		val++;

		t->priv.regs[reg] &= 0xd0;
		t->priv.regs[reg] |= (val & 0x2f);

		ret = r850_write_regs(t, 0x14, &t->priv.regs[0x14], 2);
		if (ret)
			break;

		ret = r850_read_adc_value(t, &tmp);
		if (ret)
			break;

		if (imr_tmp.value > tmp) {
			if (direction == R850_IMR_DIRECTION_GAIN)
				imr_tmp.gain = val;
			else
				imr_tmp.phase = val;

			imr_tmp.value = tmp;
		} else if ((imr_tmp.value + 2) < tmp) {	/* (imr_tmp.value < (tmp - 2)) */
			break;
		}
	}

	if (!ret)
		*imr = imr_tmp;

	return ret;
}

static int r850_imr_check_section(struct r850_tuner *t, struct r850_imr *imr)
{
	int ret = 0, i, n = 0;
	struct r850_imr imr_points[3];
	u8 val = 0xff;

	imr_points[1].gain = imr->gain;

	if (imr->gain) {
		imr_points[0].gain = imr->gain - 1;
		imr_points[2].gain = imr->gain + 1;
	} else {
		imr_points[0].gain = (imr->gain & 0xdf) + 1;
		imr_points[2].gain = (imr->gain | 0x20) + 1;
	}

	imr_points[0].phase = imr->phase;
	imr_points[1].phase = imr->phase;
	imr_points[2].phase = imr->phase;

	for (i = 0; i < 3; i++) {
		ret = r850_imr_check_iq_tree(t,
					     &imr_points[i],
					     R850_IMR_DIRECTION_PHASE, 3);
		if (ret)
			break;

		if (val > imr_points[i].value) {
			val = imr_points[i].value;
			n = i;
		}
	}

	if (!ret)
		*imr = imr_points[n];

	return ret;
}

static int r850_imr_check_iqcap(struct r850_tuner *t, struct r850_imr *imr)
{
	int ret = 0, i;

	t->priv.regs[0x14] &= 0xd0;
	t->priv.regs[0x14] |= (imr->gain & 0x2f);

	ret = r850_write_regs(t, 0x14, &t->priv.regs[0x14], 1);
	if (ret)
		return ret;

	t->priv.regs[0x15] &= 0xd0;
	t->priv.regs[0x15] |= (imr->phase & 0x2f);

	imr->iqcap = 0;
	imr->value = 0xff;

	for (i = 0; i < 3; i++) {
		u8 tmp;

		t->priv.regs[0x15] &= 0x3f;
		t->priv.regs[0x15] |= ((i << 6) & 0xc0);

		ret = r850_write_regs(t, 0x15, &t->priv.regs[0x15], 1);
		if (ret)
			break;

		ret = r850_read_adc_value(t, &tmp);
		if (ret)
			break;

		if (tmp < imr->value) {
			imr->iqcap = i;
			imr->value = tmp;
		}
	}

	return ret;
}

static int r850_prepare_calibration(struct r850_tuner *t,
				    enum r850_calibration cal)
{
	switch (cal) {
	case R850_CALIBRATION_IMR:
		memcpy(t->priv.regs, imr_cal_regs, sizeof(t->priv.regs));
		break;

	case R850_CALIBRATION_LPF:
		memcpy(t->priv.regs, lpf_cal_regs, sizeof(t->priv.regs));
		break;

	default:
		return -EINVAL;
	}

#if 0
	return r850_write_regs(t, 0x08,
			       &t->priv.regs[0x08], R850_NUM_REGS - 0x08);
#else
	return 0;
#endif
}

static int r850_calibrate_imr(struct r850_tuner *t)
{
	int ret = 0, i, j;
	int n[5] = { 2, 1, 0, 3, 4 };
	u8 mixer_mode, mixer_amp_lpf;

	mixer_mode = t->priv.mixer_mode;
	mixer_amp_lpf = t->priv.mixer_amp_lpf_imr_cal;

	for (i = 0; i < 5; i++) {
		u32 ring_freq;
		bool full = false;
		int pre = 2;
		struct r850_imr *imr;

		j = n[i];

		switch (j) {
		case 0:
			ring_freq = 136000;
			t->priv.regs[0x24] &= 0xf0;
			t->priv.regs[0x24] |= 0x0a;
			pre = 1;
			break;

		case 1:
			ring_freq = 326400;
			t->priv.regs[0x24] &= 0xf0;
			t->priv.regs[0x24] |= 0x05;
			break;

		case 2:
			ring_freq = 544000;
			t->priv.regs[0x24] &= 0xf0;
			t->priv.regs[0x24] |= 0x02;
			full = true;
			break;

		case 3:
			ring_freq = 816000;
			t->priv.regs[0x24] &= 0xf0;
			if (mixer_mode)
				full = true;
			break;

		case 4:
			ring_freq = 204000;
			t->priv.regs[0x24] &= 0xf0;
			t->priv.regs[0x24] |= 0x08;
			pre = 1;
			break;

		default:
			return 0;
		}

		imr = &t->priv.imr_cal[mixer_mode].imr[j];

		t->priv.regs[0x23] &= 0xa0;
		t->priv.regs[0x23] |= 0x11;

		if (!mixer_mode) {
			ret = r850_set_mux(t,
					   ring_freq - 5300, ring_freq,
					   R850_SYSTEM_UNDEFINED);
			if (ret)
				return ret;

			ret = r850_set_pll(t,
					   ring_freq - 5300, 5300,
					   R850_SYSTEM_UNDEFINED);
			if (ret)
				return ret;

			t->priv.regs[0x13] &= 0xe8;
			t->priv.regs[0x13] |= (mixer_amp_lpf & 0x07);

			ret = r850_write_regs(t, 0x13, &t->priv.regs[0x13], 1);
			if (ret)
				return ret;

			if (j == 4) {
				t->priv.regs[0x24] &= 0xcf;
				t->priv.regs[0x24] |= 0x10;
			} else {
				t->priv.regs[0x24] |= 0x30;
			}

			ret = r850_write_regs(t, 0x24, &t->priv.regs[0x24], 1);
			if (ret)
				return ret;

			t->priv.regs[0x29] &= 0xf0;
			t->priv.regs[0x29] |= 0x08;

			ret = r850_write_regs(t, 0x29, &t->priv.regs[0x29], 1);
			if (ret)
				return ret;
		} else {
			ret = r850_set_mux(t,
					   ring_freq + 5300, ring_freq,
					   R850_SYSTEM_UNDEFINED);
			if (ret)
				return ret;

			ret = r850_set_pll(t,
					   ring_freq + 5300, 5300,
					   R850_SYSTEM_UNDEFINED);
			if (ret)
				return ret;

			t->priv.regs[0x13] |= 0x10;
			t->priv.regs[0x13] &= 0xf8;
			t->priv.regs[0x13] |= (mixer_amp_lpf & 0x07);

			ret = r850_write_regs(t, 0x13, &t->priv.regs[0x13], 1);
			if (ret)
				return ret;

			t->priv.regs[0x29] &= 0xf0;

			if (j == 4) {
				t->priv.regs[0x29] |= 0x07;

				t->priv.regs[0x24] &= 0xcf;
				t->priv.regs[0x24] |= 0x10;
			} else {
				t->priv.regs[0x29] |= 0x06;

				t->priv.regs[0x24] |= 0x30;
			}

			ret = r850_write_regs(t, 0x29, &t->priv.regs[0x29], 1);
			if (ret)
				return ret;

			ret = r850_write_regs(t, 0x24, &t->priv.regs[0x24], 1);
			if (ret)
				return ret;
		}

		t->priv.regs[0x29] |= 0xf0;

		ret = r850_write_regs(t, 0x29, &t->priv.regs[0x29], 1);
		if (ret)
			return ret;

		if (full) {
			enum r850_imr_direction d;

			memset(imr, 0 ,sizeof(*imr));

			ret = r850_imr_check_iq_cross(t, imr, &d);
			if (ret)
				return ret;

			ret = r850_imr_check_iq_step(t, imr, d);
			if (ret)
				return ret;

			ret = r850_imr_check_iq_tree(t, imr,
						     (d == R850_IMR_DIRECTION_GAIN)
							 ? R850_IMR_DIRECTION_PHASE
							 : R850_IMR_DIRECTION_GAIN,
						     5);
			if (ret)
				return ret;

			ret = r850_imr_check_iq_tree(t, imr, d, 3);
			if (ret)
				return ret;
		} else {
			*imr = t->priv.imr_cal[mixer_mode].imr[pre];
		}

		ret = r850_imr_check_section(t, imr);
		if (ret)
			return ret;

		ret = r850_imr_check_iqcap(t, imr);
		if (ret)
			return ret;

		if (((imr->gain) & 0x0f) <= 0x06 &&
		    ((imr->phase) & 0x0f) <= 0x06)
			t->priv.imr_cal[mixer_mode].result[j] = true;
		else
			t->priv.imr_cal[mixer_mode].result[j] = false;

		if (full) {
			/* reset gain/phase/iqcap */
			t->priv.regs[0x14] &= 0xd0;
			t->priv.regs[0x15] &= 0x10;

			ret = r850_write_regs(t, 0x14, &t->priv.regs[0x14], 2);
			if (ret)
				return ret;
		}
	}

	t->priv.imr_cal[mixer_mode].done = true;
	t->priv.imr_cal[mixer_mode].mixer_amp_lpf = mixer_amp_lpf;

	return 0;
}

static int r850_calibrate_lpf(struct r850_tuner *t,
			      u32 if_freq, u8 bw,
			      u8 gap, struct r850_lpf_params *lpf)
{
	int ret = 0, i;
	u8 val, val2, val3;
	u8 bandwidth;

	ret = r850_set_pll(t,
			   72000 - if_freq, if_freq,
			   R850_SYSTEM_UNDEFINED);
	if (ret)
		return ret;

	for (i = 5; i < 16; i++) {
		t->priv.regs[0x29] &= 0x0f;
		t->priv.regs[0x29] |= ((i << 4) & 0xf0);
		ret = r850_write_regs(t, 0x29, &t->priv.regs[0x29], 1);
		if (ret)
			return ret;

		mdelay(5);

		ret = r850_read_adc_value(t, &val);
		if (ret)
			return ret;

		if (val > 0x28)
			break;
	}

	if (if_freq > 9999) {
		ret = r850_set_pll(t, 63500, 8500, R850_SYSTEM_UNDEFINED);
		if (ret)
			return ret;

		mdelay(5);

		ret = r850_read_adc_value(t, &val3);
		if (ret)
			return ret;

		if (val3 <= (val + 8)) {
			ret = r850_set_pll(t,
					   72000 - if_freq, if_freq,
					   R850_SYSTEM_UNDEFINED);
			if (ret)
				return ret;
		} else {
			/* failed. */
			return -EIO;
		}
	}

	for (i = (bw == 2) ? 1 : 0; i < 3; i++) {
		bandwidth = (!i) ? 0 : (i + 1);

		t->priv.regs[0x17] &= 0x9f;
		t->priv.regs[0x17] &= 0xe1;
		t->priv.regs[0x17] |= ((bandwidth << 5) & 0x60);
		ret = r850_write_regs(t, 0x17, &t->priv.regs[0x17], 1);
		if (ret)
			return ret;

		mdelay(5);

		ret = r850_read_adc_value(t, &val);
		if (ret)
			return ret;

		t->priv.regs[0x17] &= 0xe1;
		t->priv.regs[0x17] |= 0x1a;
		ret = r850_write_regs(t, 0x17, &t->priv.regs[0x17], 1);
		if (ret)
			return ret;

		mdelay(5);

		ret = r850_read_adc_value(t, &val2);
		if (ret)
			return ret;

		if ((val2 + 16) < val)
			break;
	}

	lpf->bandwidth = bandwidth;
	lpf->lsb = 0;

	for (i = 0; i < 16; i++) {
		t->priv.regs[0x17] &= 0xe1;
		t->priv.regs[0x17] |= ((i << 1) & 0x1e);
		ret = r850_write_regs(t, 0x17, &t->priv.regs[0x17], 1);
		if (ret)
			return ret;

		mdelay(5);

		ret = r850_read_adc_value(t, &val2);
		if (ret)
			return ret;

		if (!i) {
			if (if_freq <= 9999)
				val = val2;
			else
				val = val3;
		}

		if ((val2 + gap) < val) {
			if (!i)
				return -EIO;

			t->priv.regs[0x17] &= 0xe0;
			t->priv.regs[0x17] |= 1 | (((i - 1) << 1) & 0x1e);
			ret = r850_write_regs(t, 0x17, &t->priv.regs[0x17], 1);
			if (ret)
				return ret;

			mdelay(5);

			ret = r850_read_adc_value(t, &val2);
			if (ret)
				return ret;

			if ((val2 + gap) < val) {
				i--;
				lpf->lsb = 1;
			}

			break;
		}
	}

	lpf->code = i;

	return 0;
}

static int r850_set_system_params(struct r850_tuner *t)
{
	int ret = 0;

	if (t->priv.sys.system == R850_SYSTEM_UNDEFINED)
		return -EINVAL;

	if (!t->config.no_imr_calibration &&
	    (!t->priv.imr_cal[t->priv.mixer_mode].done ||
	     t->priv.imr_cal[t->priv.mixer_mode].mixer_amp_lpf != t->priv.mixer_amp_lpf_imr_cal)) {
		ret = r850_prepare_calibration(t, R850_CALIBRATION_IMR);
		if (ret)
			return ret;

		ret = r850_calibrate_imr(t);
		if (ret)
			return ret;
	}

	if (memcmp(&t->priv.sys, &t->priv.sys_curr,
		   sizeof(struct r850_system_config))) {
		int i;
		struct r850_system_config *sys = &t->priv.sys;
		const struct r850_system_params *prm = NULL;
		struct r850_lpf_params lpf;

		for (i = 0; i < sys_param_num[sys->system][t->priv.chip]; i++) {
			const struct r850_system_params *p = &sys_params[sys->system][t->priv.chip][i];

			if (p->bandwidth == sys->bandwidth &&
			    p->if_freq == sys->if_freq) {
				prm = p;
				break;
			}
		}

		if (!prm)
			return -EINVAL;

		if (!t->config.no_lpf_calibration) {
			ret = r850_prepare_calibration(t,
						       R850_CALIBRATION_LPF);
			if (ret)
				return ret;

			ret = r850_calibrate_lpf(t,
						 prm->filt_cal_if, prm->bw,
						 2, &lpf);
			if (ret)
				return ret;
		} else {
			lpf = prm->lpf;
		}

		r850_init_regs(t);

		t->priv.regs[0x17] = 0x00;
		t->priv.regs[0x17] |= (lpf.lsb & 0x01);
		t->priv.regs[0x17] |= ((lpf.code << 1) & 0x1e);
		t->priv.regs[0x17] |= ((lpf.bandwidth << 5) & 0x60);
		t->priv.regs[0x17] |= ((prm->hpf_notch << 7) & 0x80);

		t->priv.regs[0x18] &= 0x0f;
		t->priv.regs[0x18] |= ((prm->hpf_cor << 4) & 0xf0);

		t->priv.regs[0x12] &= 0xbf;
		t->priv.regs[0x12] |= ((prm->filt_ext_ena << 6) & 0x40);

		t->priv.regs[0x18] &= 0xf3;
		t->priv.regs[0x18] |= ((prm->filt_comp << 2) & 0x0c);

		t->priv.regs[0x2f] &= 0xf3;
		t->priv.regs[0x2f] |= ((prm->agc_clk << 2) & 0x0c);

		if (t->priv.chip) {
			t->priv.regs[0x2c] &= 0xfe;
			t->priv.regs[0x2c] |= ((prm->img_gain >> 1) & 0x01);
		}

		t->priv.regs[0x2e] &= 0xef;
		t->priv.regs[0x2e] |= ((prm->img_gain << 4) & 0x10);

#if 0
		ret = r850_write_regs(t, 0x08,
				      &t->priv.regs[0x08], R850_NUM_REGS - 0x08);
		if (ret)
			return ret;
#endif

		t->priv.sys_curr = t->priv.sys;
	}

	return 0;
}

static int r850_set_system_frequency(struct r850_tuner *t, u32 rf_freq)
{
	int ret = 0, i;
	const struct r850_system_frequency_params *prm_p = NULL;
	struct r850_system_frequency_params prm;
	u32 lo_freq;

	for (i = 0; i < sys_freq_param_num[t->priv.sys_curr.system]; i++) {
		const struct r850_system_frequency_params *p = &sys_freq_params[t->priv.sys_curr.system][i];

		if ((!p->if_freq || p->if_freq == t->priv.sys_curr.if_freq) &&
		    (!p->rf_freq_min || p->rf_freq_min <= rf_freq) &&
		    (!p->rf_freq_max || p->rf_freq_max >= rf_freq)) {
			prm_p = p;
			break;
		}
	}

	if (!prm_p)
		return -EINVAL;

	prm = *prm_p;

	switch (t->priv.sys_curr.system) {
	case R850_SYSTEM_DVB_C:
	case R850_SYSTEM_J83B:
	case R850_SYSTEM_ISDB_T:
		if (t->priv.chip)
			prm.filter_top = 6;
		break;

	default:
		break;
	}

	t->priv.regs[0x13] &= 0xef;
	if (t->priv.mixer_mode) {
		t->priv.regs[0x13] |= 0x10;
		lo_freq = rf_freq - t->priv.sys_curr.if_freq;
	} else {
		lo_freq = rf_freq + t->priv.sys_curr.if_freq;
	}

	t->priv.regs[0x0a] &= 0xbf;
	t->priv.regs[0x0a] |= ((prm.na_pwr_det << 6) & 0x40);

	t->priv.regs[0x10] &= 0xdf;
	t->priv.regs[0x10] |= (init_regs[0x0c] & 0x20);

	t->priv.regs[0x0b] &= 0x7f;
	t->priv.regs[0x0b] |= ((prm.lna_nrb_det << 7) & 0x80);

	t->priv.regs[0x26] &= 0xf8;
	t->priv.regs[0x26] |= ((7 - prm.lna_top) & 0x07);

	t->priv.regs[0x27] = prm.lna_vtl_h;

	t->priv.regs[0x11] &= 0xef;
	t->priv.regs[0x11] |= ((prm.rf_lte_psg << 4) & 0x10);

	t->priv.regs[0x26] &= 0x8f;
	t->priv.regs[0x26] |= (((7- prm.rf_top) << 4) & 0x70);

	t->priv.regs[0x2a] = prm.rf_vtl_h;

	if (prm.rf_gain_limit <= 3) {
		if (prm.rf_gain_limit < 2)
			t->priv.regs[0x12] &= 0xfb;
		else
			t->priv.regs[0x12] |= 0x02;

		if (prm.rf_gain_limit % 2)
			t->priv.regs[0x10] |= 0x40;
		else
			t->priv.regs[0x10] &= 0xbf;
	}

	t->priv.regs[0x13] &= 0xf8;
	t->priv.regs[0x13] |= (prm.mixer_amp_lpf & 0x07);

	t->priv.regs[0x28] &= 0xf0;
	t->priv.regs[0x28] |= ((15 - prm.mixer_top) & 0x0f);

	if (t->priv.chip) {
		t->priv.regs[0x2c] &= 0xf1;
		t->priv.regs[0x2c] |= (((7 - prm.filter_top) << 1) & 0x0e);
	} else {
		t->priv.regs[0x2c] &= 0xf0;
		t->priv.regs[0x2c] |= ((15 - prm.filter_top) & 0x0f);
	}

	t->priv.regs[0x0a] &= 0xef;
	t->priv.regs[0x0a] |= ((prm.filt_3th_lpf_cur << 4) & 0x10);

	t->priv.regs[0x18] &= 0xfc;
	t->priv.regs[0x18] |= (prm.filt_3th_lpf_gain & 0x03);

	t->priv.regs[0x29] = (((prm.filter_vth << 4) & 0xf0) |
			      (prm.mixer_vth & 0x0f));
	t->priv.regs[0x2b] = (((prm.filter_vtl << 4) & 0xf0) |
			      (prm.mixer_vtl & 0x0f));

	t->priv.regs[0x16] &= 0x3f;
	t->priv.regs[0x16] |= ((prm.mixer_gain_limit << 6) & 0xc0);

	t->priv.regs[0x2e] &= 0x7f;
	t->priv.regs[0x2e] |= ((prm.mixer_detbw_lpf << 7) & 0x80);

	switch (prm.lna_rf_dis_mode) {
	case 1:
		t->priv.regs[0x2d] |= 0x03;
		t->priv.regs[0x1f] |= 0x01;
		t->priv.regs[0x20] |= 0x20;
		break;

	case 2:
		t->priv.regs[0x2d] |= 0x03;
		t->priv.regs[0x1f] &= 0xfe;
		t->priv.regs[0x20] &= 0xdf;
		break;

	case 3:
		t->priv.regs[0x2d] |= 0x03;
		t->priv.regs[0x1f] |= 0x01;
		t->priv.regs[0x20] &= 0xdf;
		break;

	case 4:
		t->priv.regs[0x2d] |= 0x03;
		t->priv.regs[0x1f] &= 0xfe;
		t->priv.regs[0x20] |= 0x20;
		break;

	default:
		t->priv.regs[0x2d] &= 0xfc;
		t->priv.regs[0x1f] |= 0x01;
		t->priv.regs[0x20] |= 0x20;
		break;
	}

	t->priv.regs[0x1f] &= 0xfd;
	t->priv.regs[0x1f] |= ((prm.lna_rf_charge_cur << 1) & 0x02);

	t->priv.regs[0x0d] &= 0xdf;
	t->priv.regs[0x0d] |= ((prm.lna_rf_dis_curr << 5) & 0x20);

	t->priv.regs[0x2d] &= 0x0f;
	t->priv.regs[0x2d] |= ((prm.rf_dis_slow_fast << 4) & 0xf0);

	t->priv.regs[0x2c] &= 0x0f;
	t->priv.regs[0x2c] |= ((prm.lna_dis_slow_fast << 4) & 0xf0);

	t->priv.regs[0x19] &= 0xbf;
	t->priv.regs[0x19] |= ((prm.bb_dis_curr << 6) & 0x40);

	t->priv.regs[0x25] &= 0x3b;
	t->priv.regs[0x25] |= (((prm.mixer_filter_dis << 6) & 0xc0) |
			       ((prm.bb_det_mode << 2) & 0x04));

	t->priv.regs[0x19] &= 0xfd;
	t->priv.regs[0x19] |= ((prm.enb_poly_gain << 1) & 0x02);

	t->priv.regs[0x28] &= 0x0f;
	t->priv.regs[0x28] |= (((15 - prm.nrb_top) << 4) & 0xf0);

	t->priv.regs[0x1a] &= 0x33;
	t->priv.regs[0x1a] |= (((prm.nrb_bw_lpf << 6) & 0xc0) |
			       ((prm.nrb_bw_hpf << 2) & 0x0c));
	t->priv.regs[0x1a] |= (((prm.nrb_bw_lpf << 6) & 0xc0) |
			       ((prm.nrb_bw_hpf << 2) & 0x0c));

	t->priv.regs[0x2e] &= 0xf3;
	t->priv.regs[0x2e] |= ((prm.img_nrb_adder << 2) & 0x0c);

	t->priv.regs[0x0d] &= 0xf9;
	t->priv.regs[0x0d] |= ((prm.hpf_comp << 1) & 0x06);

	t->priv.regs[0x15] &= 0xef;
	t->priv.regs[0x15] |= ((prm.fb_res_1st << 4) & 0x10);

#if 1
	if ((rf_freq - 478000) <= 3999 &&
	    t->priv.sys_curr.system == R850_SYSTEM_ISDB_T)
		t->priv.regs[0x2f] &= 0xf3;
#endif

	t->priv.regs[0x19] &= 0xdf;

	if (t->config.loop_through) {
		t->priv.regs[0x08] |= 0xc0;
		t->priv.regs[0x0a] |= 0x02;
	} else {
		t->priv.regs[0x08] &= 0x3f;
		t->priv.regs[0x08] |= 0x40;
		t->priv.regs[0x0a] &= 0xfd;
	}

	if (t->config.clock_out)
		t->priv.regs[0x22] &= 0xfb;
	else
		t->priv.regs[0x22] |= 0x04;

	ret = r850_set_mux(t, rf_freq, lo_freq, t->priv.sys_curr.system);
	if (ret)
		return ret;

	return r850_set_pll(t,
			    lo_freq, t->priv.sys_curr.if_freq,
			    t->priv.sys_curr.system);
}

static int r850_check_xtal_power(struct r850_tuner *t)
{
	int ret = 0, i;
	u8 bank = 55, pwr = 3;		/* xtal: 24MHz */

	r850_init_regs(t);

	t->priv.regs[0x2f] &= (t->priv.chip) ? 0xfd : 0xfc;

	t->priv.regs[0x1b] &= 0x80;
	t->priv.regs[0x1b] |= 0x12;

	t->priv.regs[0x1e] &= 0xe0;
	t->priv.regs[0x1e] |= 0x08;

	t->priv.regs[0x22] &= 0x27;

	t->priv.regs[0x1d] &= 0x0f;

	t->priv.regs[0x21] |= 0xf8;

	t->priv.regs[0x22] &= 0x77;
	t->priv.regs[0x22] |= 0x80;

	t->priv.regs[0x1f] &= 0x80;
	t->priv.regs[0x1f] |= 0x40;

	t->priv.regs[0x1f] &= 0xbf;

	ret = r850_write_regs(t, 0x08,
			      &t->priv.regs[0x08], R850_NUM_REGS - 0x08);
	if (ret)
		return ret;

	for (i = 0; i <= 3; i++) {
		u8 tmp;

		t->priv.regs[0x22] &= 0xcf;
		t->priv.regs[0x22] |= (i << 4);

		ret = r850_write_regs(t, 0x22, &t->priv.regs[0x22], 1);
		if (ret)
			break;

		ret = r850_read_regs(t, 0x02, &tmp, 1);
		if (ret)
			break;

		if ((tmp & 0x40) && (((tmp & 0x3f) - (bank - 6)) <= 12)) {
			pwr = i;
			break;
		}
	}

	if (!ret) {
		if (pwr < 3)
			pwr++;

		t->priv.xtal_pwr = pwr;
	}

	return ret;
}

int r850_init(struct r850_tuner *t)
{
	int ret = 0, i;
	u8 regs[R850_NUM_REGS];

	mutex_init(&t->priv.lock);

	t->priv.init = false;

	t->priv.chip = 0;
	t->priv.sleep = false;

	t->priv.sys.system = R850_SYSTEM_UNDEFINED;

	t->priv.imr_cal[0].done = false;
	t->priv.imr_cal[1].done = false;

	t->priv.sys_curr.system = R850_SYSTEM_UNDEFINED;

	for (i = 0; i < 4; i++) {
		u8 tmp;

		ret = r850_read_regs(t, 0x00, &tmp, 1);
		if (ret) {
			dev_err(t->dev,
				"r850_init: r850_read_regs(0x00) failed. (ret: %d)\n",
				ret);
			continue;
		}

		if (tmp & 0x98) {
			t->priv.chip = 1;
			break;
		}
	}

	if (ret)
		return ret;

	ret = r850_read_regs(t, 0x08, &regs[0x08], R850_NUM_REGS - 0x08);
	if (ret) {
		dev_err(t->dev,
			"r850_init: r850_read_regs(0x08-0x2f) failed. (ret: %d)\n",
			ret);
		return ret;
	}

	ret = r850_check_xtal_power(t);
	if (ret)
		return ret;

	ret = r850_write_regs(t, 0x08, &regs[0x08], R850_NUM_REGS - 0x08);
	if (ret)
		return ret;

	r850_init_regs(t);

	t->priv.init = true;

	return ret;
}

int r850_term(struct r850_tuner *t)
{
	if (!t->priv.init)
		return 0;

	t->priv.sys.system = R850_SYSTEM_UNDEFINED;

	t->priv.imr_cal[0].done = false;
	t->priv.imr_cal[1].done = false;

	t->priv.sys_curr.system = R850_SYSTEM_UNDEFINED;

	memset(t->priv.regs, 0, sizeof(t->priv.regs));

	t->priv.chip = 0;

	mutex_destroy(&t->priv.lock);

	t->priv.init = false;

	return 0;
}

int r850_sleep(struct r850_tuner *t)
{
	int ret = 0;

	if (!t->priv.init)
		return -EINVAL;

#if 0
	mutex_lock(&t->priv.lock);

	if (t->priv.sleep)
		goto exit;

#if 0
	t->priv.regs[0x08] &= 0xc0;
	t->priv.regs[0x08] |= 0x03;

	t->priv.regs[0x09] = 0xee;

	t->priv.regs[0x0a] &= 0x02;
	t->priv.regs[0x0a] |= 0xb9;

	t->priv.regs[0x0b] = 0xfe;

	t->priv.regs[0x0c] |= 0x0f;

	t->priv.regs[0x08] &= 0x3f;
	t->priv.regs[0x0c] &= 0xfd;

	if (!t->config.loop_through)
		t->priv.regs[0x08] |= 0x40;

	t->priv.regs[0x0d] |= 0x21;

	t->priv.regs[0x27] |= 0xf0;

	t->priv.regs[0x0e] &= 0xf3;
	t->priv.regs[0x0e] |= 0x04;

	t->priv.regs[0x19] |= 0x04;

	t->priv.regs[0x11] |= 0x40;

	t->priv.regs[0x2a] &= 0x0f;

	t->priv.regs[0x08] |= 0x30;
#else
	memcpy(t->priv.regs, sleep_regs, sizeof(t->priv.regs));

	if (!t->config.loop_through)
		t->priv.regs[0x08] |= 0x40;
#endif

	ret = r850_write_regs(t, 0x08,
			      &t->priv.regs[0x08], R850_NUM_REGS - 0x08);
	if (!ret)
		t->priv.sleep = true;

	t->priv.sys_curr.system = R850_SYSTEM_UNDEFINED;

exit:
	mutex_unlock(&t->priv.lock);
#endif

	return ret;
}

int r850_wakeup(struct r850_tuner *t)
{
	int ret = 0;

	if (!t->priv.init)
		return -EINVAL;

#if 0
	mutex_lock(&t->priv.lock);

	if (!t->priv.sleep)
		goto exit;

#if 0
	t->priv.regs[0x09] |= 0x20;
	t->priv.regs[0x0a] |= 0x80;
	t->priv.regs[0x0b] |= 0x3c;
	t->priv.regs[0x0c] |= 0xc0;
#else
	memcpy(t->priv.regs, wakeup_regs, sizeof(t->priv.regs));
#endif

	ret = r850_write_regs(t, 0x08,
			      &t->priv.regs[0x08], R850_NUM_REGS - 0x08);
	if (ret)
		goto exit;

	r850_init_regs(t);

	ret = r850_write_regs(t, 0x08,
			      &t->priv.regs[0x08], R850_NUM_REGS - 0x08);
	if (!ret)
		t->priv.sleep = false;

exit:
	mutex_unlock(&t->priv.lock);
#endif

	return ret;
}

int r850_set_system(struct r850_tuner *t, struct r850_system_config *system)
{
	u8 mixer_mode, mixer_amp_lpf_imr_cal;

	if (!t->priv.init)
		return -EINVAL;

	switch (system->system) {
	case R850_SYSTEM_DVB_T:
	case R850_SYSTEM_DVB_T2:
	case R850_SYSTEM_DVB_T2_1:
	case R850_SYSTEM_DVB_C:
	case R850_SYSTEM_FM:
		mixer_mode = 1;
		mixer_amp_lpf_imr_cal = 4;
		break;

	case R850_SYSTEM_J83B:
	case R850_SYSTEM_DTMB:
	case R850_SYSTEM_ATSC:
		mixer_mode = 0;
		mixer_amp_lpf_imr_cal = 7;
		break;

	case R850_SYSTEM_ISDB_T:
		mixer_mode = 1;
		mixer_amp_lpf_imr_cal = 7;
		break;

	default:
		return -EINVAL;
	}

	mutex_lock(&t->priv.lock);

	t->priv.sys = *system;
	t->priv.mixer_mode = mixer_mode;
	t->priv.mixer_amp_lpf_imr_cal = mixer_amp_lpf_imr_cal;

	t->priv.sys_curr.system = R850_SYSTEM_UNDEFINED;

	mutex_unlock(&t->priv.lock);

	return 0;
}

int r850_set_frequency(struct r850_tuner *t, u32 freq)
{
	int ret = 0;

	if (!t->priv.init)
		return -EINVAL;

	if (freq < 40000 || freq > 1002000)
		return -EINVAL;

	mutex_lock(&t->priv.lock);

	ret = r850_set_system_params(t);
	if (ret)
		goto exit;

	ret = r850_set_system_frequency(t, freq);

exit:
	mutex_unlock(&t->priv.lock);

	return ret;
}

int r850_is_pll_locked(struct r850_tuner *t, bool *locked)
{
	int ret = 0;
	u8 tmp = 0;

	if (!t->priv.init)
		return -EINVAL;

	mutex_lock(&t->priv.lock);

	ret = r850_read_regs(t, 0x02, &tmp, 1);

	mutex_unlock(&t->priv.lock);

	if (ret) {
		dev_err(t->dev,
			"r850_is_pll_locked: r850_read_regs() failed. (ret: %d)\n",
			ret);
		return ret;
	}

	*locked = (tmp & 0x40) ? true : false;

	return 0;
}
