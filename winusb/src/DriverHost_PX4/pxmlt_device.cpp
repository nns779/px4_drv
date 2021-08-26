// pxmlt_device.cpp

#include "pxmlt_device.hpp"

#include <cassert>
#include <cmath>

#include "type.hpp"
#include "command.hpp"
#include "misc_win.h"

namespace px4 {

const PxMltDevice::PxMltDeviceParam PxMltDevice::params_[][5] = {
	/* PX-MLT5U */
	{ { 0x65, 3, 4 }, { 0x6c, 1, 3 }, { 0x64, 1, 1 }, { 0x6c, 3, 2 }, { 0x64, 3, 0 } },
	/* PX-MLT5PE */
	{ { 0x65, 3, 0 }, { 0x6c, 1, 1 }, { 0x64, 1, 2 }, { 0x6c, 3, 3 }, { 0x64, 3, 4 } },
	/* PX-MLT8PE3 */
	{ { 0x65, 3, 0 }, { 0x6c, 3, 3 }, { 0x64, 3, 4 }, { 0x00, 0, 1 }, { 0x00, 0, 2 } },
	/* PX-MLT8PE5 */
	{ { 0x65, 1, 0 }, { 0x64, 1, 1 }, { 0x6c, 1, 2 }, { 0x6c, 3, 3 }, { 0x64, 3, 4 } },
	/* ISDB6014 V2.0 (4TS) */
	{ { 0x65, 3, 0 }, { 0x6c, 1, 1 }, { 0x64, 1, 2 }, { 0x64, 3, 4 }, { 0x00, 0, 3 } }
};

PxMltDevice::PxMltDevice(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager)
	: DeviceBase(path, device_def, index, receiver_manager),
	config_(),
	lock_(),
	available_(true),
	init_(false),
	open_count_(0),
	lnb_power_count_(0),
	streaming_count_(0),
	tuner_lock_{}
{
	if (usb_dev_.descriptor.idVendor != 0x0511)
		throw DeviceError("px4::PxMltDevice::PxMltDevice: unsupported device. (unknown vendor id)");

	receiver_num_ = 5;

	switch (usb_dev_.descriptor.idProduct) {
	case 0x084e:
		model_ = PxMltDeviceModel::PXMLT5U;
		break;

	case 0x24e:
		model_ = PxMltDeviceModel::PXMLT5PE;
		break;

	case 0x0252:
		model_ = PxMltDeviceModel::PXMLT8PE3;
		receiver_num_ = 3;
		break;

	case 0x0253:
		model_ = PxMltDeviceModel::PXMLT8PE5;
		break;

	case 0x0254:
		model_ = PxMltDeviceModel::ISDB6014_4TS;
		receiver_num_ = 4;
		break;

	default:
		throw DeviceError("px4::PxMltDevice::PxMltDevice: unsupported device. (unknown product id)");
	}

	LoadConfig();

	memset(&it930x_, 0, sizeof(it930x_));
	memset(&stream_ctx_, 0, sizeof(stream_ctx_));
}

PxMltDevice::~PxMltDevice()
{
	Term();
}

void PxMltDevice::LoadConfig()
{
	auto &configs = device_def_.configs;

	if (configs.Exists(L"XferPackets"))
		config_.usb.xfer_packets = px4::util::wtoui(configs.Get(L"XferPackets"));

	if (configs.Exists(L"UrbMaxPackets"))
		config_.usb.urb_max_packets = px4::util::wtoui(configs.Get(L"UrbMaxPackets"));

	if (configs.Exists(L"MaxUrbs"))
		config_.usb.max_urbs = px4::util::wtoui(configs.Get(L"MaxUrbs"));

	if (configs.Exists(L"NoRawIo"))
		config_.usb.no_raw_io = px4::util::wtob(configs.Get(L"NoRawIo"));

	if (configs.Exists(L"ReceiverMaxPackets"))
		config_.device.receiver_max_packets = px4::util::wtoui(configs.Get(L"ReceiverMaxPackets"));

	if (configs.Exists(L"PsbPurgeTimeout"))
		config_.device.psb_purge_timeout = px4::util::wtoi(configs.Get(L"PsbPurgeTimeout"));

	if (configs.Exists(L"DiscardNullPackets"))
		config_.device.discard_null_packets = px4::util::wtob(configs.Get(L"DiscardNullPackets"));

	dev_dbg(&dev_, "px4::PxMltDevice::LoadConfig: xfer_packets: %u\n", config_.usb.xfer_packets);
	dev_dbg(&dev_, "px4::PxMltDevice::LoadConfig: urb_max_packets: %u\n", config_.usb.urb_max_packets);
	dev_dbg(&dev_, "px4::PxMltDevice::LoadConfig: max_urbs: %u\n", config_.usb.max_urbs);
	dev_dbg(&dev_, "px4::PxMltDevice::LoadConfig: no_raw_io: %s\n", (config_.usb.no_raw_io) ? "true" : "false");
	dev_dbg(&dev_, "px4::PxMltDevice::LoadConfig: receiver_max_packets: %u\n", config_.device.receiver_max_packets);
	dev_dbg(&dev_, "px4::PxMltDevice::LoadConfig: psb_purge_timeout: %i\n", config_.device.psb_purge_timeout);
	dev_dbg(&dev_, "px4::PxMltDevice::LoadConfig: discard_null_packets: %s\n", (config_.device.discard_null_packets) ? "true" : "false");

	return;
}

int PxMltDevice::Init()
{
	dev_dbg(&dev_, "px4::PxMltDevice::Init: init_: %s\n", (init_) ? "true" : "false");

	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (init_)
		return -EALREADY;

	int ret = 0;
	itedtv_bus &bus = it930x_.bus;

	bus.dev = &dev_;
	bus.type = ITEDTV_BUS_USB;
	bus.usb.dev = &usb_dev_;
	bus.usb.ctrl_timeout = 3000;

	it930x_.dev = &dev_;
	it930x_.config.xfer_size = 188 * config_.usb.xfer_packets;
	it930x_.config.i2c_speed = 0x07;

	ret = itedtv_bus_init(&bus);
	if (ret)
		goto fail_bus;

	ret = it930x_init(&it930x_);
	if (ret)
		goto fail_bridge;

	for (int i = 0; i < receiver_num_; i++) {
		it930x_stream_input &input = it930x_.config.input[i];

		input.enable = true;
		input.is_parallel = false;
		input.port_number = params_[static_cast<int>(model_)][i].port_number;
		input.slave_number = i;
		input.i2c_bus = params_[static_cast<int>(model_)][i].i2c_bus;
		input.i2c_addr = params_[static_cast<int>(model_)][i].i2c_addr;
		input.packet_len = 188;
		input.sync_byte = ((i + 1) << 4) | 0x07;

		receivers_[i].reset(new PxMltReceiver(*this, i));
		stream_ctx_.stream_buf[i] = receivers_[i]->GetStreamBuffer();
	}

	for (int i = receiver_num_; i < 5; i++) {
		it930x_.config.input[i].enable = false;
		it930x_.config.input[i].port_number = params_[static_cast<int>(model_)][i].port_number;
	}

	ret = it930x_raise(&it930x_);
	if (ret)
		goto fail_device;

	ret = it930x_load_firmware(&it930x_, "it930x-firmware.bin");
	if (ret)
		goto fail_device;

	ret = it930x_init_warm(&it930x_);
	if (ret)
		goto fail_device;

	/* GPIO */
	ret = it930x_set_gpio_mode(&it930x_, 7, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(&it930x_, 7, true);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(&it930x_, 2, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(&it930x_, 2, false);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(&it930x_, 11, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	/* LNB power supply: off */
	ret = it930x_write_gpio(&it930x_, 11, false);
	if (ret)
		goto fail_device;

	if (config_.device.discard_null_packets) {
		it930x_pid_filter filter;

		filter.block = true;
		filter.num = 1;
		filter.pid[0] = 0x1fff;

		for (int i = 0; i < receiver_num_; i++) {
			ret = it930x_set_pid_filter(&it930x_, i, &filter);
			if (ret)
				goto fail_device;
		}
	}

	init_ = true;

	for (auto it = device_def_.receivers.cbegin(); it != device_def_.receivers.cend(); ++it) {
		px4::command::ReceiverInfo ri;

		if (it->index < 0 || it->index >= receiver_num_)
			continue;

		wcscpy_s(ri.device_name, device_def_.name.c_str());
		ri.device_guid = device_def_.guid;
		wcscpy_s(ri.receiver_name, it->name.c_str());
		ri.receiver_guid = it->guid;
		ri.systems = it->systems;
		ri.index = it->index;
		ri.data_id = 0;

		receiver_manager_.Register(ri, receivers_[it->index].get());
	}

	return 0;

fail_device:
	for (int i = 0; i < receiver_num_; i++) {
		stream_ctx_.stream_buf[i] = nullptr;
		receivers_[i].reset();
	}

	it930x_term(&it930x_);

fail_bridge:
	itedtv_bus_term(&bus);

fail_bus:
	return ret;
}

void PxMltDevice::Term()
{
	dev_dbg(&dev_, "px4::PxMltDevice::Term: init_: %s\n", (init_) ? "true" : "false");

	std::unique_lock<std::recursive_mutex> lock(lock_);

	if (!init_)
		return;

	for (int i = 0; i < receiver_num_; i++)
		receiver_manager_.Unregister(receivers_[i].get());

	lock.unlock();

	for (int i = 0; i < receiver_num_; i++) {
		stream_ctx_.stream_buf[i] = nullptr;
		receivers_[i].reset();
	}

	lock.lock();

	it930x_term(&it930x_);
	itedtv_bus_term(&it930x_.bus);

	init_ = false;
	return;
}

void PxMltDevice::SetAvailability(bool available)
{
	available_ = available;
}

ReceiverBase* PxMltDevice::GetReceiver(int id) const
{
	if (id < 0 || id >= receiver_num_)
		throw std::out_of_range("receiver id out of range");

	return receivers_[id].get();
}

const i2c_comm_master& PxMltDevice::GetI2cMaster(int bus) const
{
	if (bus < 1 || bus > 3)
		throw std::out_of_range("bus number out of range");

	return it930x_.i2c_master[bus - 1];
}

int PxMltDevice::SetBackendPower(bool state)
{
	dev_dbg(&dev_, "px4::PxMltDevice::SetBackendPower: state: %s\n", (state) ? "true" : "false");

	int ret = 0;
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!state && !available_)
		return 0;

	if (state) {
		ret = it930x_write_gpio(&it930x_, 7, false);
		if (ret)
			return ret;

		Sleep(80);

		ret = it930x_write_gpio(&it930x_, 2, true);
		if (ret)
			return ret;

		Sleep(20);
	} else {
		it930x_write_gpio(&it930x_, 2, false);
		it930x_write_gpio(&it930x_, 7, true);
	}

	return 0;
}

int PxMltDevice::SetLnbVoltage(std::int32_t voltage)
{
	dev_dbg(&dev_, "px4::PxMltDevice::SetLnbVoltage: voltage: %d\n", voltage);

	int ret = 0;
	std::lock_guard<std::recursive_mutex> lock(lock_);

	assert((voltage && lnb_power_count_ >= 0) || (!voltage && lnb_power_count_ > 0));

	if (!voltage) {
		lnb_power_count_--;
		if (!available_)
			return 0;
	}

	if (!lnb_power_count_)
		ret = it930x_write_gpio(&it930x_, 11, !!voltage);

	if (voltage && !ret)
		lnb_power_count_++;

	return ret;
}

int PxMltDevice::StartCapture()
{
	dev_dbg(&dev_, "px4::PxMltDevice::StartCapture\n");

	int ret = 0;
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!streaming_count_) {
		ret = it930x_purge_psb(&it930x_, config_.device.psb_purge_timeout);
		if (ret) {
			dev_err(&dev_, "px4::PxMltDevice::StartCapture: it930x_purge_psb() failed. (ret: %d)\n", ret);
			if (ret != -ETIMEDOUT)
				return ret;
		}

		it930x_.bus.usb.streaming.urb_buffer_size = 188 * config_.usb.urb_max_packets;
		it930x_.bus.usb.streaming.urb_num = config_.usb.max_urbs;
		it930x_.bus.usb.streaming.no_dma = true;
		it930x_.bus.usb.streaming.no_raw_io = config_.usb.no_raw_io;

		stream_ctx_.remain_len = 0;

		ret = itedtv_bus_start_streaming(&it930x_.bus, StreamHandler, this);
		if (ret) {
			dev_err(&dev_, "px4::PxMltDevice::StartCapture: itedtv_bus_start_streaming() failed. (ret: %d)\n", ret);
			return ret;
		}
	}

	streaming_count_++;
	dev_dbg(&dev_, "px4::PxMltDevice::StartCapture: streaming_count_: %u\n", streaming_count_);

	return 0;
}

int PxMltDevice::StopCapture()
{
	dev_dbg(&dev_, "px4::PxMltDevice::StopCapture\n");

	std::lock_guard<std::recursive_mutex> lock(lock_);

	assert(streaming_count_ > 0);

	streaming_count_--;
	if (!streaming_count_) {
		dev_dbg(&dev_, "px4::PxMltDevice::StopCapture: stopping...\n");
		itedtv_bus_stop_streaming(&it930x_.bus);
	} else {
		dev_dbg(&dev_, "px4::PxMltDevice::StopCapture: streaming_count_: %u\n", streaming_count_);
	}

	return 0;
}

void PxMltDevice::StreamProcess(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf[], int num, std::uint8_t **buf, std::size_t &len)
{
	std::uint8_t *p = *buf;
	std::size_t remain = len;

	while (remain) {
		std::size_t i;
		bool sync_remain = false;

		for (i = 0; i < PXMLT_DEVICE_TS_SYNC_COUNT; i++) {
			if (((i + 1) * 188) <= remain) {
				if ((p[i * 188] & 0x8f) != 0x07)
					break;
			} else {
				sync_remain = true;
				break;
			}
		}

		if (sync_remain)
			break;

		if (i < PXMLT_DEVICE_TS_SYNC_COUNT) {
			p++;
			remain--;
			continue;
		}

		while (remain >= 188 && ((p[0] & 0x8f) == 0x07)) {
			u8 id = (p[0] & 0x70) >> 4;

			if (id && id <= num) {
				std::size_t pkt_len = 188;

				p[0] = 0x47;
				stream_buf[id - 1]->Write(p, pkt_len);
			}

			p += 188;
			remain -= 188;
		}
	}

	for (int i = 0; i < num; i++)
		stream_buf[i]->NotifyWrite();

	*buf = p;
	len = remain;

	return;
}

int PxMltDevice::StreamHandler(void *context, void *buf, std::uint32_t len)
{
	PxMltDevice &obj = *static_cast<PxMltDevice*>(context);
	int receiver_num = obj.receiver_num_;
	StreamContext &stream_ctx = obj.stream_ctx_;
	std::uint8_t *p = static_cast<std::uint8_t*>(buf);
	std::size_t remain = len;

	if (stream_ctx.remain_len) {
		if ((stream_ctx.remain_len + len) >= PXMLT_DEVICE_TS_SYNC_SIZE) {
			std::uint8_t * remain_buf = stream_ctx.remain_buf;
			std::size_t t = PXMLT_DEVICE_TS_SYNC_SIZE - stream_ctx.remain_len;

			memcpy(remain_buf + stream_ctx.remain_len, p, t);
			stream_ctx.remain_len = PXMLT_DEVICE_TS_SYNC_SIZE;

			StreamProcess(stream_ctx.stream_buf, receiver_num, &remain_buf, stream_ctx.remain_len);
			if (!stream_ctx.remain_len) {
				p += t;
				remain -= t;
			}

			stream_ctx.remain_len = 0;
		} else {
			memcpy(stream_ctx.remain_buf + stream_ctx.remain_len, p, len);
			stream_ctx.remain_len += len;

			return 0;
		}
	}

	StreamProcess(stream_ctx.stream_buf, receiver_num, &p, remain);

	if (remain) {
		dev_dbg(&obj.dev_, "px4::PxMltDevice::StreamHandler: remain: %lu\n", remain);
		memcpy(stream_ctx.remain_buf, p, remain);
		stream_ctx.remain_len = remain;
	}

	return 0;
}

const PxMltDevice::PxMltReceiver::PxMltReceiverCnTableIsdbS PxMltDevice::PxMltReceiver::isdbs_cn_table_[] = {
	{ 0x5af, 0 }, { 0x597, 100 }, { 0x57e, 200 }, { 0x567, 300 },
	{ 0x550, 400 }, { 0x539, 500 }, { 0x522, 600 }, { 0x50c, 700 },
	{ 0x4f6, 800 }, { 0x4e1, 900 }, { 0x4cc, 1000 }, { 0x4b6, 1100 },
	{ 0x4a1, 1200 }, { 0x48c, 1300 }, { 0x477, 1400 }, { 0x463, 1500 },
	{ 0x44f, 1600 }, { 0x43c, 1700 }, { 0x428, 1800 }, { 0x416, 1900 },
	{ 0x403, 2000 }, { 0x3ef, 2100 }, { 0x3dc, 2200 }, { 0x3c9, 2300 },
	{ 0x3b6, 2400 }, { 0x3a4, 2500 }, { 0x392, 2600 }, { 0x381, 2700 },
	{ 0x36f, 2800 }, { 0x35f, 2900 }, { 0x34e, 3000 }, { 0x33d, 3100 },
	{ 0x32d, 3200 }, { 0x31d, 3300 }, { 0x30d, 3400 }, { 0x2fd, 3500 },
	{ 0x2ee, 3600 }, { 0x2df, 3700 }, { 0x2d0, 3800 }, { 0x2c2, 3900 },
	{ 0x2b4, 4000 }, { 0x2a6, 4100 }, { 0x299, 4200 }, { 0x28c, 4300 },
	{ 0x27f, 4400 }, { 0x272, 4500 }, { 0x265, 4600 }, { 0x259, 4700 },
	{ 0x24d, 4800 }, { 0x241, 4900 }, { 0x236, 5000 }, { 0x22b, 5100 },
	{ 0x220, 5200 }, { 0x215, 5300 }, { 0x20a, 5400 }, { 0x200, 5500 },
	{ 0x1f6, 5600 }, { 0x1ec, 5700 }, { 0x1e2, 5800 }, { 0x1d8, 5900 },
	{ 0x1cf, 6000 }, { 0x1c6, 6100 }, { 0x1bc, 6200 }, { 0x1b3, 6300 },
	{ 0x1aa, 6400 }, { 0x1a2, 6500 }, { 0x199, 6600 }, { 0x191, 6700 },
	{ 0x189, 6800 }, { 0x181, 6900 }, { 0x179, 7000 }, { 0x171, 7100 },
	{ 0x169, 7200 }, { 0x161, 7300 }, { 0x15a, 7400 }, { 0x153, 7500 },
	{ 0x14b, 7600 }, { 0x144, 7700 }, { 0x13d, 7800 }, { 0x137, 7900 },
	{ 0x130, 8000 }, { 0x12a, 8100 }, { 0x124, 8200 }, { 0x11e, 8300 },
	{ 0x118, 8400 }, { 0x112, 8500 }, { 0x10c, 8600 }, { 0x107, 8700 },
	{ 0x101, 8800 }, { 0xfc, 8900 }, { 0xf7, 9000 }, { 0xf2, 9100 },
	{ 0xec, 9200 }, { 0xe7, 9300 }, { 0xe2, 9400 }, { 0xdd, 9500 },
	{ 0xd8, 9600 }, { 0xd4, 9700 }, { 0xcf, 9800 }, { 0xca, 9900 },
	{ 0xc6, 10000 }, { 0xc2, 10100 }, { 0xbe, 10200 }, { 0xb9, 10300 },
	{ 0xb5, 10400 }, { 0xb1, 10500 }, { 0xae, 10600 }, { 0xaa, 10700 },
	{ 0xa6, 10800 }, { 0xa3, 10900 }, { 0x9f, 11000 }, { 0x9b, 11100 },
	{ 0x98, 11200 }, { 0x95, 11300 }, { 0x91, 11400 }, { 0x8e, 11500 },
	{ 0x8b, 11600 }, { 0x88, 11700 }, { 0x85, 11800 }, { 0x82, 11900 },
	{ 0x7f, 12000 }, { 0x7c, 12100 }, { 0x7a, 12200 }, { 0x77, 12300 },
	{ 0x74, 12400 }, { 0x72, 12500 }, { 0x6f, 12600 }, { 0x6d, 12700 },
	{ 0x6b, 12800 }, { 0x68, 12900 }, { 0x66, 13000 }, { 0x64, 13100 },
	{ 0x61, 13200 }, { 0x5f, 13300 }, { 0x5d, 13400 }, { 0x5b, 13500 },
	{ 0x59, 13600 }, { 0x57, 13700 }, { 0x55, 13800 }, { 0x53, 13900 },
	{ 0x51, 14000 }, { 0x4f, 14100 }, { 0x4e, 14200 }, { 0x4c, 14300 },
	{ 0x4a, 14400 }, { 0x49, 14500 }, { 0x47, 14600 }, { 0x45, 14700 },
	{ 0x44, 14800 }, { 0x42, 14900 }, { 0x41, 15000 }, { 0x3f, 15100 },
	{ 0x3e, 15200 }, { 0x3c, 15300 }, { 0x3b, 15400 }, { 0x3a, 15500 },
	{ 0x38, 15600 }, { 0x37, 15700 }, { 0x36, 15800 }, { 0x34, 15900 },
	{ 0x33, 16000 }, { 0x32, 16100 }, { 0x31, 16200 }, { 0x30, 16300 },
	{ 0x2f, 16400 }, { 0x2e, 16500 }, { 0x2d, 16600 }, { 0x2c, 16700 },
	{ 0x2b, 16800 }, { 0x2a, 16900 }, { 0x29, 17000 }, { 0x28, 17100 },
	{ 0x27, 17200 }, { 0x26, 17300 }, { 0x25, 17400 }, { 0x24, 17500 },
	{ 0x23, 17600 }, { 0x22, 17800 }, { 0x21, 17900 }, { 0x20, 18000 },
	{ 0x1f, 18200 }, { 0x1e, 18300 }, { 0x1d, 18500 }, { 0x1c, 18700 },
	{ 0x1b, 18900 }, { 0x1a, 19000 }, { 0x19, 19200 }, { 0x18, 19300 },
	{ 0x17, 19500 }, { 0x16, 19700 }, { 0x15, 19900 }, { 0x14, 20000 }
};

PxMltDevice::PxMltReceiver::PxMltReceiver(PxMltDevice &parent, std::uintptr_t index)
	: ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_BEFORE_TUNE),
	parent_(parent),
	index_(index),
	lock_(),
	open_(false),
	close_cond_(),
	lnb_power_(false),
	current_system_(px4::SystemType::UNSPECIFIED),
	streaming_(false)
{
	auto i2c_addr = parent_.params_[static_cast<int>(parent_.model_)][index_].i2c_addr;
	auto i2c_bus = parent_.params_[static_cast<int>(parent_.model_)][index_].i2c_bus;

	tuner_lock_ = &parent_.tuner_lock_[(i2c_bus == 3) ? 0 : 1];

	const device *dev = &parent_.GetDevice();
	const i2c_comm_master *i2c = &parent_.GetI2cMaster(i2c_bus);

	cxd2856er_.dev = dev;
	cxd2856er_.i2c = i2c;
	cxd2856er_.i2c_addr.slvx = i2c_addr + 2;
	cxd2856er_.i2c_addr.slvt = i2c_addr;
	cxd2856er_.config.xtal = 24000;
	cxd2856er_.config.tuner_i2c = true;

	cxd2858er_.dev = dev;
	cxd2858er_.i2c = &cxd2856er_.i2c_master;
	cxd2858er_.i2c_addr = 0x60;
	cxd2858er_.config.xtal = 16000;
	cxd2858er_.config.ter.lna = true;
	cxd2858er_.config.sat.lna = true;
}

PxMltDevice::PxMltReceiver::~PxMltReceiver()
{
	dev_dbg(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::~PxMltReceiver(%u)\n", index_);

	std::unique_lock<std::mutex> lock(lock_);

	close_cond_.wait(lock, [this] { return !open_; });

	dev_dbg(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::~PxMltReceiver(%u): exit\n", index_);
}

int PxMltDevice::PxMltReceiver::Open()
{
	dev_dbg(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::Open(%u): open_: %s\n", index_, (open_) ? "true" : "false");

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);
	std::lock_guard<std::recursive_mutex> dev_lock(parent_.lock_);

	if (open_)
		return -EALREADY;

	if (!parent_.open_count_) {
		ret = parent_.SetBackendPower(true);
		if (ret) {
			dev_err(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::Open(%u): SetBackendPower(true) failed. (ret: %d)\n", index_, ret);
			goto fail_power;
		}
	}

	ret = cxd2856er_init(&cxd2856er_);
	if (ret) {
		dev_err(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::Open(%u): cxd2856er_init() failed. (ret: %d)\n", index_, ret);
		goto fail_demod_init;
	}

	{
		std::lock_guard<std::mutex> tuner_lock(*tuner_lock_);

		ret = cxd2858er_init(&cxd2858er_);
		if (ret) {
			dev_err(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::Open(%u): cxd2858er_init() failed. (ret: %d)\n", index_, ret);
			goto fail_tuner_init;
		}
	}

	ret = cxd2856er_write_slvt_reg(&cxd2856er_, 0x00, 0x00);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&cxd2856er_, 0xc4, 0x80, 0x88);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&cxd2856er_, 0xc5, 0x01, 0x01);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&cxd2856er_, 0xc6, 0x03, 0x1f);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg(&cxd2856er_, 0x00, 0x60);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&cxd2856er_, 0x52, 0x03, 0x1f);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg(&cxd2856er_, 0x00, 0x00);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&cxd2856er_, 0xc8, 0x03, 0x1f);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&cxd2856er_, 0xc9, 0x03, 0x1f);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg(&cxd2856er_, 0x00, 0xa0);
	if (ret)
		goto fail_backend;

	ret = cxd2856er_write_slvt_reg_mask(&cxd2856er_, 0xb9, 0x01, 0x01);
	if (ret)
		goto fail_backend;

	current_system_ = px4::SystemType::UNSPECIFIED;

	parent_.open_count_++;
	open_ = true;

	return 0;

fail_backend:
	{
		std::lock_guard<std::mutex> tuner_lock(*tuner_lock_);
		cxd2858er_term(&cxd2858er_);
	}

fail_tuner_init:
	cxd2856er_term(&cxd2856er_);

fail_demod_init:
	if (!parent_.open_count_)
		parent_.SetBackendPower(true);

fail_power:
	return ret;
}

void PxMltDevice::PxMltReceiver::Close()
{
	dev_dbg(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::Close(%u): open_: %s\n", index_, (open_) ? "true" : "false");

	if (!open_)
		return;

	SetCapture(false);
	SetLnbVoltage(0);

	std::unique_lock<std::mutex> lock(lock_);
	std::lock_guard<std::recursive_mutex> dev_lock(parent_.lock_);

	assert(parent_.open_count_ > 0);

	{
		std::lock_guard<std::mutex> tuner_lock(*tuner_lock_);
		cxd2858er_term(&cxd2858er_);
	}

	cxd2856er_term(&cxd2856er_);

	parent_.open_count_--;
	if (!parent_.open_count_)
		parent_.SetBackendPower(false);

	open_ = false;

	lock.unlock();
	close_cond_.notify_all();
	
	return;
}

int PxMltDevice::PxMltReceiver::SetFrequency()
{
	if (!open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::SetFrequency(%u): freq: %u\n", index_, params_.freq);

	int ret = 0;
	cxd2856er_system_params demod_params = { 0 };
	std::unique_lock<std::mutex> lock(lock_);

	current_system_ = px4::SystemType::UNSPECIFIED;

	switch (params_.system) {
	case px4::SystemType::ISDB_T:
		demod_params.bandwidth = (params_.bandwidth) ? params_.bandwidth : 6;

		ret = cxd2856er_wakeup(&cxd2856er_, CXD2856ER_ISDB_T_SYSTEM, &demod_params);
		if (ret)
			dev_err(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::SetFrequency(%u): cxd2856er_wakeup(CXD2856ER_ISDB_T_SYSTEM) failed. (ret: %d)\n", index_, ret);

		break;

	case px4::SystemType::ISDB_S:
		ret = cxd2856er_wakeup(&cxd2856er_, CXD2856ER_ISDB_S_SYSTEM, &demod_params);
		if (ret)
			dev_err(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::SetFrequency(%u): cxd2856er_wakeup(CXD2856ER_ISDB_S_SYSTEM) failed. (ret: %d)\n", index_, ret);

		break;

	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		return ret;

	{
		std::lock_guard<std::mutex> tuner_lock(*tuner_lock_);

		switch (params_.system) {
		case px4::SystemType::ISDB_T:
			ret = cxd2858er_set_params_t(&cxd2858er_, CXD2858ER_ISDB_T_SYSTEM, params_.freq, 6);
			if (ret)
				dev_err(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::SetFrequency(%u): cxd2858er_set_params_t(%u, 6) failed. (ret: %d)\n");

			break;

		case px4::SystemType::ISDB_S:
			ret = cxd2858er_set_params_s(&cxd2858er_, CXD2858ER_ISDB_S_SYSTEM, params_.freq, 28860);
			if (ret)
				dev_err(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::SetFrequency(%u): cxd2858er_set_params_s(%u, 28860) failed. (ret: %d)\n");

			break;

		default:
			break;
		}
	}

	if (ret)
		return ret;

	ret = cxd2856er_post_tune(&cxd2856er_);
	if (ret) {
		dev_err(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::SetFrequency(%u): cxd2856er_post_tune() failed. (ret: %d)\n");
		return ret;
	}

	current_system_ = params_.system;
	return ret;
}

int PxMltDevice::PxMltReceiver::CheckLock(bool &locked)
{
	if (!open_)
		return -EINVAL;

	int ret = 0;
	bool unlocked = false;
	std::lock_guard<std::mutex> lock(lock_);

	switch (current_system_) {
	case px4::SystemType::ISDB_T:
	{
		bool unlocked = false;

		ret = cxd2856er_is_ts_locked_isdbt(&cxd2856er_, &locked, &unlocked);
		if (!ret && unlocked)
			ret = -ECANCELED;

		break;
	}

	case px4::SystemType::ISDB_S:
		ret = cxd2856er_is_ts_locked_isdbs(&cxd2856er_, &locked);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

int PxMltDevice::PxMltReceiver::SetStreamId()
{
	if (params_.system != px4::SystemType::ISDB_S)
		return -EINVAL;

	if (!open_)
		return -EINVAL;

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	if (params_.stream_id < 12) {
		ret = cxd2856er_set_slot_isdbs(&cxd2856er_, params_.stream_id);
		if (ret)
			dev_err(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::SetStreamId(%u): cxd2856er_set_slot_isdbs(%u) failed. (ret: %d)\n", index_, params_.stream_id, ret);
	} else {
		ret = cxd2856er_set_tsid_isdbs(&cxd2856er_, params_.stream_id);
		if (ret)
			dev_err(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::SetStreamId(%u): cxd2856er_set_tsid_isdbs(%u) failed. (ret: %d)\n", index_, params_.stream_id, ret);
	}

	return ret;
}

int PxMltDevice::PxMltReceiver::SetLnbVoltage(std::int32_t voltage)
{
	if (!open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::SetLnbVoltage(%u): voltage: %d\n", index_, voltage);

	if (voltage != 0 && voltage != 15)
		return -EINVAL;

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	if (lnb_power_ == !!voltage)
		return 0;

	ret = parent_.SetLnbVoltage(voltage);
	if (!ret || !voltage)
		lnb_power_ = !!voltage;

	return ret;
}

int PxMltDevice::PxMltReceiver::SetCapture(bool capture)
{
	if (!open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::PxMltDevice::PxMltReceiver::SetCapture(%u): capture: %s\n", index_, (capture) ? "true" : "false");

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	if ((capture && streaming_) || (!capture && !streaming_))
		return -EALREADY;

	if (capture)
		ret = parent_.StartCapture();
	else
		ret = parent_.StopCapture();

	if (!ret) {
		if (capture) {
			std::size_t size = 188 * parent_.config_.device.receiver_max_packets;

			stream_buf_->Alloc(size);
			stream_buf_->SetThresholdSize(size / 10);
			stream_buf_->Start();
		} else {
			stream_buf_->Stop();
		}
		streaming_ = capture;
	}

	return ret;
}

int PxMltDevice::PxMltReceiver::ReadStat(px4::command::StatType type, std::int32_t &value)
{
	if (!open_)
		return -EINVAL;

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	value = 0;

	switch (type) {
	case px4::command::StatType::SIGNAL_STRENGTH:
		// not implemented
		ret = -ENOSYS;
		break;

	case px4::command::StatType::CNR:
		switch (current_system_) {
		case px4::SystemType::ISDB_T:
		{
			std::uint16_t val;

			ret = cxd2856er_read_cnr_raw_isdbt(&cxd2856er_, &val);
			if (ret)
				break;

			value = static_cast<std::int32_t>((std::log10(val) * 10000) - 9031);
			break;
		}

		case px4::SystemType::ISDB_S:
		{
			int i, i_min, i_max;
			std::uint16_t val;

			ret = cxd2856er_read_cnr_raw_isdbs(&cxd2856er_, &val);
			if (ret)
				break;

			i_min = 0;
			i_max = (sizeof(isdbs_cn_table_) / sizeof(isdbs_cn_table_[0])) - 1;

			if (isdbs_cn_table_[i_min].val <= val) {
				value = isdbs_cn_table_[i_min].cnr;
				break;
			}
			if (isdbs_cn_table_[i_max].val >= val) {
				value = isdbs_cn_table_[i_max].cnr;
				break;
			}

			while (true) {
				i = i_min + (i_max - i_min) / 2;

				if (isdbs_cn_table_[i].val == val) {
					value = isdbs_cn_table_[i].cnr;
					break;
				}

				if (isdbs_cn_table_[i].val > val)
					i_min = i + 1;
				else
					i_max = i - 1;

				if (i_max < i_min) {
					value = isdbs_cn_table_[i_max].cnr;
					break;
				}
			}

			break;
		}

		default:
			ret = -EINVAL;
			break;
		}

		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

} // namespace px4
