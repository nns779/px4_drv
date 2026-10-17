// isdb2056_device.cpp

#include "isdb2056_device.hpp"

#include <cassert>
#include <cmath>

#include "type.hpp"
#include "command.hpp"
#include "misc_win.h"
#include "ts_sync.h"

namespace px4 {

Isdb2056Device::Isdb2056Device(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager)
	: DeviceBase(path, device_def, index, receiver_manager),
	config_(),
	lock_(),
	available_(true),
	init_(false),
	streaming_count_(0),
	card_open_(false),
	receiver_open_(false)
{
	if (usb_dev_.descriptor.idVendor != 0x0511)
		throw DeviceError("px4::Isdb2056Device::Isdb2056Device: unsupported device. (unknown vendor id)");

	switch (usb_dev_.descriptor.idProduct) {
	case 0x004b:
		model_ = Isdb2056DeviceModel::ISDB2056;
		break;

	case 0x084b:
		model_ = Isdb2056DeviceModel::ISDB2056N;
		break;

	case 0x0854:
		model_ = Isdb2056DeviceModel::PXM1UR;
		break;

	case 0x0855:
		model_ = Isdb2056DeviceModel::PXS1UR;
		break;

	default:
		model_ = Isdb2056DeviceModel::OTHER;
	}

	LoadConfig();

	memset(&it930x_, 0, sizeof(it930x_));
}

Isdb2056Device::~Isdb2056Device()
{
	Term();
}

void Isdb2056Device::LoadConfig()
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

	dev_dbg(&dev_, "px4::Isdb2056Device::LoadConfig: model: %d\n", model_);
	dev_dbg(&dev_, "px4::Isdb2056Device::LoadConfig: xfer_packets: %u\n", config_.usb.xfer_packets);
	dev_dbg(&dev_, "px4::Isdb2056Device::LoadConfig: urb_max_packets: %u\n", config_.usb.urb_max_packets);
	dev_dbg(&dev_, "px4::Isdb2056Device::LoadConfig: max_urbs: %u\n", config_.usb.max_urbs);
	dev_dbg(&dev_, "px4::Isdb2056Device::LoadConfig: no_raw_io: %s\n", (config_.usb.no_raw_io) ? "true" : "false");
	dev_dbg(&dev_, "px4::Isdb2056Device::LoadConfig: receiver_max_packets: %u\n", config_.device.receiver_max_packets);
	dev_dbg(&dev_, "px4::Isdb2056Device::LoadConfig: psb_purge_timeout: %i\n", config_.device.psb_purge_timeout);
	dev_dbg(&dev_, "px4::Isdb2056Device::LoadConfig: discard_null_packets: %s\n", (config_.device.discard_null_packets) ? "true" : "false");

	return;
}

int Isdb2056Device::Init()
{
	dev_dbg(&dev_, "px4::Isdb2056Device::Init: init_: %s\n", (init_) ? "true" : "false");

	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (init_)
		return -EALREADY;

	int ret = 0;
	itedtv_bus &bus = it930x_.bus;
	it930x_stream_input& input = it930x_.config.input[0];

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

	input.enable = true;
	input.is_parallel = false;
	input.port_number = 0;
	input.slave_number = 0;
	input.i2c_bus = 3;
	input.i2c_addr = 0x10;
	input.packet_len = 188;
	input.sync_byte = 0x47;

	for (int i = 1; i < 5; i++) {
		it930x_.config.input[i].enable = false;
		it930x_.config.input[i].port_number = i;
	}

	receiver_.reset(new Isdb2056Receiver(*this, 0));
	stream_ctx_.stream_buf = receiver_->GetStreamBuffer();

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
	ret = it930x_set_gpio_mode(&it930x_, 3, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(&it930x_, 3, true);
	if (ret)
		goto fail_device;

	ret = it930x_set_gpio_mode(&it930x_, 2, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	ret = it930x_write_gpio(&it930x_, 2, false);
	if (ret)
		goto fail_device;

#if 0
	ret = it930x_set_gpio_mode(&it930x_, 11, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	/* LNB power supply: off */
	ret = it930x_write_gpio(&it930x_, 11, false);
	if (ret)
		goto fail_device;
#endif

	if (config_.device.discard_null_packets) {
		it930x_pid_filter filter;

		filter.block = true;
		filter.num = 1;
		filter.pid[0] = 0x1fff;

		ret = it930x_set_pid_filter(&it930x_, 0, &filter);
		if (ret)
			goto fail_device;
	}

	init_ = true;

	for (auto it = device_def_.receivers.cbegin(); it != device_def_.receivers.cend(); ++it) {
		px4::command::ReceiverInfo ri;

		wcscpy_s(ri.device_name, device_def_.name.c_str());
		ri.device_guid = device_def_.guid;
		wcscpy_s(ri.receiver_name, it->name.c_str());
		ri.receiver_guid = it->guid;
		ri.systems = it->systems;
		ri.index = it->index;
		ri.data_id = 0;

		receiver_manager_.Register(ri, receiver_.get());
	}

	return 0;

fail_device:
	stream_ctx_.stream_buf = nullptr;
	receiver_.reset();

	it930x_term(&it930x_);

fail_bridge:
	itedtv_bus_term(&bus);

fail_bus:
	return ret;
}

void Isdb2056Device::Term()
{
	dev_dbg(&dev_, "px4::Isdb2056Device::Term: init_: %s\n", (init_) ? "true" : "false");

	std::unique_lock<std::recursive_mutex> lock(lock_);

	if (!init_)
		return;

	receiver_manager_.Unregister(receiver_.get());

	lock.unlock();

	stream_ctx_.stream_buf = nullptr;
	receiver_.reset();

	lock.lock();

	it930x_term(&it930x_);
	itedtv_bus_term(&it930x_.bus);

	init_ = false;
	return;
}

void Isdb2056Device::SetAvailability(bool available)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	/* 切断後の古いカードセッションから USB 制御転送を発行しない */
	if (available_ && !available)
		card_open_ = false;
	available_ = available;
}

ReceiverBase* Isdb2056Device::GetReceiver(int id) const
{
	if (id != 0)
		throw std::out_of_range("receiver id out of range");

	return receiver_.get();
}

int Isdb2056Device::OpenCard()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!available_)
		return -ENODEV;
	if (card_open_)
		return -EBUSY;

	/* 受信中の電源再投入は TS を途切れさせるため、未使用時だけ投入する */
	if (!receiver_open_) {
		int ret = SetBackendPower(true);
		if (ret)
			return ret;
	}

	int ret = it930x_bcas_init(&it930x_);
	if (ret) {
		if (!receiver_open_)
			SetBackendPower(false);
		return ret;
	}

	card_open_ = true;
	return 0;
}

void Isdb2056Device::CloseCard()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!card_open_)
		return;

	card_open_ = false;
	if (!receiver_open_)
		SetBackendPower(false);
}

int Isdb2056Device::DetectCard(bool &detected)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return available_ ? it930x_bcas_detect_card(&it930x_, &detected) : -ENODEV;
}

int Isdb2056Device::ResetCard()
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ ? it930x_bcas_reset_card(&it930x_) : -ENODEV;
}

int Isdb2056Device::SetCardBaudrate(std::uint32_t baudrate)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ ? it930x_bcas_set_baudrate(&it930x_, baudrate) : -ENODEV;
}

int Isdb2056Device::IsCardDataReady(bool &ready)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ ? it930x_bcas_check_ready(&it930x_, &ready) : -ENODEV;
}

int Isdb2056Device::ReadCardData(std::uint8_t *buf, std::uint8_t &len)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ ? it930x_bcas_get_data(&it930x_, buf, &len) : -ENODEV;
}

int Isdb2056Device::WriteCardData(const std::uint8_t *buf, std::uint8_t len)
{
	std::lock_guard<std::recursive_mutex> lock(lock_);
	return card_open_ ? it930x_bcas_send_data(&it930x_, buf, len) : -ENODEV;
}

const i2c_comm_master& Isdb2056Device::GetI2cMaster(int bus) const
{
	if (bus < 1 || bus > 3)
		throw std::out_of_range("bus number out of range");

	return it930x_.i2c_master[bus - 1];
}

int Isdb2056Device::SetBackendPower(bool state)
{
	dev_dbg(&dev_, "px4::Isdb2056Device::SetBackendPower: %s\n", (state) ? "true" : "false");

	int ret = 0;
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!state && !available_)
		return 0;

	if (state) {
		ret = it930x_write_gpio(&it930x_, 3, false);
		if (ret)
			return ret;

		Sleep(100);

		ret = it930x_write_gpio(&it930x_, 2, true);
		if (ret)
			return ret;

		Sleep(20);
	} else {
		it930x_write_gpio(&it930x_, 2, false);
		it930x_write_gpio(&it930x_, 3, true);
	}

	return 0;
}

int Isdb2056Device::SetLnbVoltage(std::int32_t voltage)
{
	dev_dbg(&dev_, "px4::Isdb2056Device::SetLnbVoltage: voltage: %d\n", voltage);

	return 0;
}

int Isdb2056Device::PrepareCapture()
{
	dev_dbg(&dev_, "px4::Isdb2056Device::PrepareCapture\n");

	int ret = 0;
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (streaming_count_)
		return 0;

	ret = it930x_purge_psb(&it930x_, config_.device.psb_purge_timeout);
	if (ret)
		dev_err(&dev_, "px4::Isdb2056Device::PrepareCapture: it930x_purge_psb() failed. (ret: %d)\n", ret);

	return (ret && ret != -ETIMEDOUT) ? ret : 0;
}

int Isdb2056Device::StartCapture()
{
	dev_dbg(&dev_, "px4::Isdb2056Device::StartCapture\n");

	int ret = 0;
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (!streaming_count_) {
		it930x_.bus.usb.streaming.urb_buffer_size = 188 * config_.usb.urb_max_packets;
		it930x_.bus.usb.streaming.urb_num = config_.usb.max_urbs;
		it930x_.bus.usb.streaming.no_dma = true;
		it930x_.bus.usb.streaming.no_raw_io = config_.usb.no_raw_io;

		stream_ctx_.remain_len = 0;

		ret = itedtv_bus_start_streaming(&it930x_.bus, StreamHandler, this);
		if (ret) {
			dev_err(&dev_, "px4::Isdb2056Device::StartCapture: itedtv_bus_start_streaming() failed. (ret: %d)\n", ret);
			return ret;
		}
		streaming_count_++;
	}

	dev_dbg(&dev_, "px4::Isdb2056Device::StartCapture: streaming_count_: %u\n", streaming_count_);

	return 0;
}

int Isdb2056Device::StopCapture()
{
	dev_dbg(&dev_, "px4::Isdb2056Device::StopCapture\n");

	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (streaming_count_) {
		dev_dbg(&dev_, "px4::Isdb2056Device::StopCapture: stopping...\n");
		itedtv_bus_stop_streaming(&it930x_.bus);
		streaming_count_ = 0;
	}

	return 0;
}

void Isdb2056Device::StreamProcess(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf, std::uint8_t **buf, std::size_t &len)
{
	std::uint8_t *p = *buf;
	std::size_t remain = len;

	while (remain) {
		std::size_t i = 0;
		bool sync_remain = false;

		while (true) {
			if (((i + 1) * 188) <= remain) {
				/* 連続する同期バイトを数え、崩れた位置の直前までを出力する */
				if (!px4_ts_has_plain_sync(p[i * 188]))
					break;
			} else {
				sync_remain = true;
				break;
			}
			i++;
		}

		if (i < ISDB2056_DEVICE_TS_SYNC_COUNT) {
			p++;
			remain--;
			continue;
		}

		std::size_t pkt_len = 188 * i;
		stream_buf->Write(p, pkt_len);

		p += 188 * i;
		remain -= 188 * i;

		if (sync_remain)
			break;
	}

	stream_buf->NotifyWrite();

	*buf = p;
	len = remain;

	return;
}

int Isdb2056Device::StreamHandler(void *context, void *buf, std::uint32_t len)
{
	Isdb2056Device &obj = *static_cast<Isdb2056Device*>(context);
	StreamContext &stream_ctx = obj.stream_ctx_;
	std::uint8_t *p = static_cast<std::uint8_t*>(buf);
	std::size_t remain = len;

	if (stream_ctx.remain_len) {
		if ((stream_ctx.remain_len + len) >= ISDB2056_DEVICE_TS_SYNC_SIZE) {
			std::uint8_t * remain_buf = stream_ctx.remain_buf;
			std::size_t t = ISDB2056_DEVICE_TS_SYNC_SIZE - stream_ctx.remain_len;

			memcpy(remain_buf + stream_ctx.remain_len, p, t);
			stream_ctx.remain_len = ISDB2056_DEVICE_TS_SYNC_SIZE;

			StreamProcess(stream_ctx.stream_buf, &remain_buf, stream_ctx.remain_len);
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

	StreamProcess(stream_ctx.stream_buf, &p, remain);

	if (remain) {
		memcpy(stream_ctx.remain_buf, p, remain);
		stream_ctx.remain_len = remain;
	}

	return 0;
}

struct tc90522_regbuf Isdb2056Device::Isdb2056Receiver::tc_init_t_[] = {
	{ 0xb0, NULL, { 0xa0 } },
	{ 0xb2, NULL, { 0x3d } },
	{ 0xb3, NULL, { 0x25 } },
	{ 0xb4, NULL, { 0x8b } },
	{ 0xb5, NULL, { 0x4b } },
	{ 0xb6, NULL, { 0x3f } },
	{ 0xb7, NULL, { 0xff } },
	{ 0xb8, NULL, { 0xc0 } },
};

struct tc90522_regbuf Isdb2056Device::Isdb2056Receiver::tc_init_s_[] = {
	{ 0x15, NULL, { 0x00 } },
	{ 0x1d, NULL, { 0x00 } },
};

Isdb2056Device::Isdb2056Receiver::Isdb2056Receiver(Isdb2056Device &parent, std::uintptr_t index)
	: ReceiverBase(RECEIVER_SAT_SET_STREAM_ID_AFTER_TUNE | RECEIVER_WAIT_AFTER_LOCK_TC_T),
	parent_(parent),
	index_(index),
	lock_(),
	init_(false),
	open_(false),
	lnb_power_(false),
	streaming_(false)
{
	memset(&r850_, 0, sizeof(r850_));
	memset(&rt710_, 0, sizeof(rt710_));

	current_system_ = px4::SystemType::UNSPECIFIED;

	const device *dev = &parent_.GetDevice();
	const i2c_comm_master *i2c = &parent_.GetI2cMaster(parent_.it930x_.config.input[0].i2c_bus);

	tc90522_t_.dev = dev;
	tc90522_t_.i2c = i2c;
	tc90522_t_.i2c_addr = 0x10;
	tc90522_t_.is_secondary = false;

	tc90522_s0_.dev = dev;
	tc90522_s0_.i2c = i2c;
	tc90522_s0_.i2c_addr = 0x11;
	tc90522_s0_.is_secondary = false;

	tc90522_s_.dev = dev;
	tc90522_s_.i2c = i2c;
	tc90522_s_.i2c_addr = (parent_.model_ == px4::Isdb2056DeviceModel::ISDB2056N) ? 0x13 : 0x11;
	tc90522_s_.is_secondary = (parent_.model_ == px4::Isdb2056DeviceModel::ISDB2056N) ? true : false;

	r850_.dev = dev;
	r850_.i2c = &tc90522_t_.i2c_master;
	r850_.i2c_addr = 0x7c;
	r850_.config.xtal = 24000;
	r850_.config.loop_through = !tc90522_t_.is_secondary;
	r850_.config.clock_out = false;
	r850_.config.no_imr_calibration = true;
	r850_.config.no_lpf_calibration = true;

	rt710_.dev = dev;
	rt710_.i2c = &tc90522_s_.i2c_master;
	rt710_.i2c_addr = 0x7a;
	rt710_.config.xtal = 24000;
	rt710_.config.loop_through = false;
	rt710_.config.clock_out = false;
	rt710_.config.signal_output_mode = RT710_SIGNAL_OUTPUT_DIFFERENTIAL;
	rt710_.config.agc_mode = RT710_AGC_POSITIVE;
	rt710_.config.vga_atten_mode = RT710_VGA_ATTEN_OFF;
	rt710_.config.fine_gain = RT710_FINE_GAIN_3DB;
	rt710_.config.scan_mode = RT710_SCAN_MANUAL;
}

Isdb2056Device::Isdb2056Receiver::~Isdb2056Receiver()
{
	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::~Isdb2056Receiver(%u)\n", index_);

	std::unique_lock<std::mutex> lock(lock_);

	close_cond_.wait(lock, [this] { return !open_; });

	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::~Isdb2056Receiver(%u): exit\n", index_);
}

int Isdb2056Device::Isdb2056Receiver::Init(bool sleep)
{
	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Init(%u): init_: %s, sleep: %s\n", index_, (init_) ? "true" : "false", (sleep) ? "true" : "false");

	int ret = 0;
	std::lock_guard<std::recursive_mutex> dev_lock(parent_.lock_);

	if (init_)
		return 0;

	ret = tc90522_init(&tc90522_t_);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Init(%u): tc90522_init() (t) failed. (ret: %d)\n", index_, ret);
		return ret;
	}

	ret = tc90522_init(&tc90522_s0_);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Init(%u): tc90522_init() (s0) failed. (ret: %d)\n", index_, ret);
		return ret;
	}

	ret = tc90522_init(&tc90522_s_);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Init(%u): tc90522_init() (s) failed. (ret: %d)\n", index_, ret);
		return ret;
	}

	ret = r850_init(&r850_);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Init(%u): r850_init() failed. (ret: %d)\n", index_, ret);
		return ret;
	}

	if (sleep) {
		ret = r850_sleep(&r850_);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Init(%u): r850_sleep() failed. (ret: %d)\n", index_, ret);
			return ret;
		}

		ret = tc90522_sleep_t(&tc90522_t_, true);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Init(%u): tc90522_sleep_t(true) failed. (ret: %d)\n", index_, ret);
			return ret;
		}
	}

	switch (parent_.model_) {
	case px4::Isdb2056DeviceModel::ISDB2056:
	case px4::Isdb2056DeviceModel::ISDB2056N:
	case px4::Isdb2056DeviceModel::PXM1UR:
	case px4::Isdb2056DeviceModel::OTHER:
	{
		ret = rt710_init(&rt710_);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Init(%u): rt710_init() failed. (ret: %d)\n", index_, ret);
			return ret;
		}

		if (sleep) {
			ret = rt710_sleep(&rt710_);
			if (ret) {
				dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Init(%u): rt710_sleep() failed. (ret: %d)\n", index_, ret);
				return ret;
			}

			ret = tc90522_sleep_s(&tc90522_s_, true);
			if (ret) {
				dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Init(%u): tc90522_sleep_s(true) failed. (ret: %d)\n", index_, ret);
				return ret;
			}
		}
		break;
	}

	default:
		break;
	}

	if (!ret)
		init_.store(true);

	return ret;
}

void Isdb2056Device::Isdb2056Receiver::Term()
{
	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Term(%u): init_: %s\n", index_, (init_) ? "true" : "false");

	std::lock_guard<std::recursive_mutex> dev_lock(parent_.lock_);

	if (!init_)
		return;

	r850_term(&r850_);

	switch (parent_.model_) {
	case px4::Isdb2056DeviceModel::ISDB2056:
	case px4::Isdb2056DeviceModel::ISDB2056N:
	case px4::Isdb2056DeviceModel::PXM1UR:
	case px4::Isdb2056DeviceModel::OTHER:
		rt710_term(&rt710_);
		break;
	default:
		break;
	}

	tc90522_term(&tc90522_t_);
	tc90522_term(&tc90522_s0_);
	tc90522_term(&tc90522_s_);

	init_ = false;

	return;
}

int Isdb2056Device::Isdb2056Receiver::Open()
{
	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Open(%u): init_: %s, open_: %s\n", index_, (init_) ? "true" : "false", (open_) ? "true" : "false");

	int ret = 0;
	std::unique_lock<std::mutex> lock(lock_);
	std::unique_lock<std::recursive_mutex> dev_lock(parent_.lock_);

	if (open_)
		return (!init_) ? -EINVAL : -EALREADY;

	if (!parent_.card_open_)
		ret = parent_.SetBackendPower(true);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Open(%u): parent_.SetBackendPower(true) failed. (ret: %d)\n", index_, ret);
		return ret;
	}

	ret = parent_.receiver_->Init(true);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Open(%u): parent_.reciver_->Init(true) failed. (ret: %d)\n", index_, ret);
		if (!parent_.card_open_)
			parent_.SetBackendPower(false);
		return ret;
	}

	ret = tc90522_write_multiple_regs(&tc90522_t_, tc_init_t_, ARRAY_SIZE(tc_init_t_));
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Open(%u): tc90522_write_multiple_regs(tc_init_t) failed. (ret: %d)\n", index_, ret);
		goto fail;
	}

	ret = tc90522_enable_ts_pins_t(&tc90522_t_, false);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Open(%u): tc90522_enable_ts_pins_t(false) failed. (ret: %d)\n", index_, ret);
		goto fail;
	}

	ret = tc90522_sleep_t(&tc90522_t_, true);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Open(%u): tc90522_sleep_t(false) failed. (ret: %d)\n", index_, ret);
		goto fail;
	}

	ret = r850_wakeup(&r850_);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Open(%u): r850_wakeup() failed. (ret: %d)\n", index_, ret);
		goto fail;
	}

	r850_system_config sys;

	sys.system = R850_SYSTEM_ISDB_T;
	sys.bandwidth = R850_BANDWIDTH_6M;
	sys.if_freq = 4063;

	ret = r850_set_system(&r850_, &sys);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Open(%u): r850_set_system() failed. (ret: %d)\n", index_, ret);
		goto fail;
	}

	ret = tc90522_write_multiple_regs(&tc90522_s_, tc_init_s_, ARRAY_SIZE(tc_init_s_));
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Open(%u): tc90522_write_multiple_regs(tc_init_s) failed. (ret: %d)\n", index_, ret);
		goto fail;
	}

	ret = tc90522_enable_ts_pins_s(&tc90522_s_, false);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Open(%u): tc90522_enable_ts_pins_s(false) failed. (ret: %d)\n", index_, ret);
		goto fail;
	}

	ret = tc90522_sleep_s(&tc90522_s_, true);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Open(%u): tc90522_sleep_s(false) failed. (ret: %d)\n", index_, ret);
		goto fail;
	}

	parent_.receiver_open_ = true;
	open_ = true;
	return ret;

fail:
	/* カード接続を維持したまま、途中まで初期化した受信回路だけを戻す */
	parent_.receiver_->Term();
	if (!parent_.card_open_)
		parent_.SetBackendPower(false);
	return ret;
}

void Isdb2056Device::Isdb2056Receiver::Close()
{
	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::Close(%u): init_: %s, open_: %s\n", index_, (init_) ? "true" : "false", (open_) ? "true" : "false");

	if (!init_ || !open_)
		return;

	SetCapture(false);
	SetLnbVoltage(0);

	std::unique_lock<std::mutex> lock(lock_);
	std::lock_guard<std::recursive_mutex> dev_lock(parent_.lock_);

	parent_.receiver_->Term();
	parent_.receiver_open_ = false;
	if (!parent_.card_open_)
		parent_.SetBackendPower(false);

	open_ = false;

	lock.unlock();
	close_cond_.notify_all();

	return;
}

int Isdb2056Device::Isdb2056Receiver::SetFrequency()
{
	if (!init_ || !open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): system: %d freq: %u\n", index_, params_.system, params_.freq);

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	switch (params_.system) {
	case px4::SystemType::ISDB_T:
	{
		ret = tc90522_write_reg(&tc90522_t_, 0x47, 0x30);
		if (ret)
			break;

		ret = tc90522_set_agc_t(&tc90522_t_, false);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): tc90522_set_agc_t(false) failed. (ret: %d)\n", index_, ret);
			break;
		}

		ret = tc90522_sleep_s(&tc90522_s_, true);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): tc90522_sleep_s(true) failed. (ret: %d)\n", index_, ret);
			break;
		}

		ret = tc90522_write_reg(&tc90522_t_, 0x0e, 0x77);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_t_, 0x0f, 0x10);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_t_, 0x71, 0x20);
		if (ret)
			break;

		ret = tc90522_sleep_t(&tc90522_t_, false);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): tc90522_sleep_t(false) failed. (ret: %d)\n", index_, ret);
			break;
		}

		ret = tc90522_write_reg(&tc90522_t_, 0x76, 0x0c);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_t_, 0x1f, 0x30);
		if (ret)
			break;

		ret = r850_wakeup(&r850_);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): r850_wakeup() failed. (ret: %d)\n", index_, ret);
			break;
		}

		ret = r850_set_frequency(&r850_, params_.freq);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): r850_set_frequency(%u) failed. (ret: %d)\n", index_, params_.freq, ret);
			break;
		}

		bool tuner_locked = false;

		for (int i = 25; i; i--) {
			ret = r850_is_pll_locked(&r850_, &tuner_locked);
			if (!ret && tuner_locked)
				break;

			Sleep(20);
		}

		if (ret)
			break;

		if (!tuner_locked) {
			dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): tuner is NOT locked.\n", index_);
			ret = -EAGAIN;
			break;
		}

		ret = tc90522_set_agc_t(&tc90522_t_, true);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): tc90522_set_agc_t(true) failed. (ret: %d)\n", index_, ret);
			break;
		}

		ret = tc90522_write_reg(&tc90522_t_, 0x71, 0x01);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_t_, 0x72, 0x25);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_t_, 0x75, 0x00);
		if (ret)
			break;

		break;
	}

	case px4::SystemType::ISDB_S:
	{
		switch (parent_.model_) {
		case px4::Isdb2056DeviceModel::PXS1UR:
			return -EINVAL;

		default:
			break;
		}

		ret = tc90522_set_agc_s(&tc90522_s_, false);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): tc90522_set_agc_s(false) failed. (ret: %d)\n", index_, ret);
			break;
		}

		ret = tc90522_write_reg(&tc90522_t_, 0x0e, 0x11);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_t_, 0x0f, 0x70);
		if (ret)
			break;

		ret = tc90522_sleep_t(&tc90522_t_, true);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): tc90522_sleep_t(true) failed. (ret: %d)\n", index_, ret);
			break;
		}

		switch (parent_.model_) {
		case px4::Isdb2056DeviceModel::ISDB2056:
		case px4::Isdb2056DeviceModel::PXM1UR:
			ret = tc90522_write_reg(&tc90522_s_, 0x07, 0x77);
			break;

		case px4::Isdb2056DeviceModel::ISDB2056N:
			ret = tc90522_write_reg(&tc90522_s0_, 0x07, 0x77);
			break;

		default:
			ret = tc90522_write_reg(&tc90522_s_, 0x07, 0x77);
			break;
		}

		if (ret)
			break;

		switch (parent_.model_) {
		case px4::Isdb2056DeviceModel::ISDB2056:
		case px4::Isdb2056DeviceModel::PXM1UR:
			ret = tc90522_write_reg(&tc90522_s_, 0x08, 0x10);
			break;

		case px4::Isdb2056DeviceModel::ISDB2056N:
			ret = tc90522_write_reg(&tc90522_s0_, 0x08, 0x37);
			break;

		default:
			ret = tc90522_write_reg(&tc90522_s_, 0x08, 0x10);
			break;
		}

		if (ret)
			break;

		ret = tc90522_sleep_s(&tc90522_s_, false);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): tc90522_sleep_s(false) failed. (ret: %d)\n", index_, ret);
			break;
		}

		ret = tc90522_write_reg(&tc90522_s_, 0x04, 0x02);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_s_, 0x8e, 0x02);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_t_, 0x1f, 0x20);
		if (ret)
			break;

		ret = rt710_set_params(&rt710_, params_.freq, 28860, 4);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): rt710_set_params(%u) failed. (ret: %d)\n", index_, params_.freq, ret);
			break;
		}

		bool tuner_locked = false;

		for (int i = 25; i; i--) {
			ret = rt710_is_pll_locked(&rt710_, &tuner_locked);
			if (!ret && tuner_locked)
				break;

			Sleep(20);
		}

		if (ret)
			break;

		if (!tuner_locked) {
			dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): tuner is NOT locked.\n", index_);
			ret = -EAGAIN;
			break;
		}

		ret = tc90522_set_agc_s(&tc90522_s_, true);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): tc90522_set_agc_s(true) failed. (ret: %d)\n", index_, ret);
			break;
		}

		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetFrequency(%u): %s.\n", index_, (!ret) ? "succeeded" : "failed");

	if (!ret) {
		current_system_ = params_.system;
	}
	else {
		current_system_ = px4::SystemType::UNSPECIFIED;
	}

	return ret;
}

int Isdb2056Device::Isdb2056Receiver::CheckLock(bool &locked)
{
	if (!init_ || !open_)
		return -EINVAL;

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	switch (current_system_) {
	case px4::SystemType::ISDB_T:
		ret = tc90522_is_signal_locked_t(&tc90522_t_, &locked);
		break;

	case px4::SystemType::ISDB_S:
		ret = tc90522_is_signal_locked_s(&tc90522_s_, &locked);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	if (locked) {
		dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::CheckLock(%u): system: %d locked: true\n", index_, current_system_);
		lock_.unlock();
		ret = SetCapture(true);
		lock_.lock();
	}

	return ret;
}

int Isdb2056Device::Isdb2056Receiver::SetStreamId()
{
	if (current_system_ != px4::SystemType::ISDB_S)
		return -EINVAL;

	if (!init_ || !open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetStreamId(%u): tsid: %d\n", index_, params_.stream_id);

	int ret = 0;
	std::uint16_t tsid;
	std::lock_guard<std::mutex> lock(lock_);

	if (params_.stream_id < 8) {
		for (int i = 50; i; i--) {
			ret = tc90522_tmcc_get_tsid_s(&tc90522_s_, params_.stream_id, &tsid);
			if ((!ret && tsid) || (ret == -EINVAL))
				break;

			Sleep(20);
		}

		if (ret)
			return ret;

		dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetStreamId(%u): slot: %u, tsid: 0x%04x\n", index_, params_.stream_id, tsid);
	} else {
		tsid = params_.stream_id;
	}

	ret = tc90522_set_tsid_s(&tc90522_s_, tsid);
	if (ret)
		return ret;

	int i;

	for (i = 50; i; i--) {
		std::uint16_t tsid2;

		ret = tc90522_get_tsid_s(&tc90522_s_, &tsid2);
		if (!ret && tsid2 == tsid)
			break;

		Sleep(20);
	}

	if (!ret && !i)
		ret = -EAGAIN;

	return ret;
}

int Isdb2056Device::Isdb2056Receiver::SetLnbVoltage(std::int32_t voltage)
{
	if (!init_ || !open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetLnbVoltage(%u): voltage: %d\n", index_, voltage);

	if (voltage != 0 && voltage != 15)
		return -EINVAL;

	return 0;
}

int Isdb2056Device::Isdb2056Receiver::SetCapture(bool capture)
{
	if (!init_ || !open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetCapture(%u): capture: %s\n", index_, (capture) ? "true" : "false");
	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetCapture(%u): streaming: %s\n", index_, (streaming_) ? "true" : "false");
	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetCapture(%u): current_system: %d\n", index_, current_system_);

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	switch (current_system_) {
	case px4::SystemType::ISDB_T:
		ret = tc90522_enable_ts_pins_t(&tc90522_t_, capture);
		if (ret)
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetCapture(%u): tc90522_enable_ts_pins_t() failed.\n", index_);

		break;

	case px4::SystemType::ISDB_S:
		ret = tc90522_enable_ts_pins_s(&tc90522_s_, capture);
		if (ret)
			dev_err(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetCapture(%u): tc90522_enable_ts_pins_s() failed.\n", index_);

		break;

	default:
		return 0;
	}

	if (ret)
		return ret;

	if (capture) {
		ret = parent_.PrepareCapture();
		if (ret)
			return ret;

		ret = parent_.StartCapture();
		if (ret)
			return ret;

		std::size_t size = 188 * parent_.config_.device.receiver_max_packets;

		if (!streaming_) {
			stream_buf_->Alloc(size);
			stream_buf_->SetThresholdSize(size / 10);
			stream_buf_->Start();
			streaming_ = true;
		} else {
			stream_buf_->Purge();
		}
	} else {
		ret = parent_.StopCapture();
		if (ret)
			return ret;

		stream_buf_->Stop();
		streaming_ = false;
	}

	dev_dbg(&parent_.dev_, "px4::Isdb2056Device::Isdb2056Receiver::SetCapture(%u): succeeded.\n", index_);

	return ret;
}

int Isdb2056Device::Isdb2056Receiver::ReadStat(px4::command::StatType type, std::int32_t &value)
{
	if (!init_ || !open_)
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
			std::uint32_t cndat;

			ret = tc90522_get_cndat_t(&tc90522_t_, &cndat);
			if (ret)
				break;

			if (!cndat)
				break;

			double p, cnr;

			p = 10 * std::log10(5505024 / (double)cndat);
			cnr = (0.024 * p * p * p * p) - (1.6 * p * p * p) + (39.8 * p * p) + (549.1 * p) + 3096.5;

			if (!std::isnan(cnr))
				value = static_cast<std::int32_t>(cnr);

			break;
		}

		case px4::SystemType::ISDB_S:
		{
			std::uint16_t cn;

			ret = tc90522_get_cn_s(&tc90522_s_, &cn);
			if (ret)
				break;

			if (cn < 3000)
				break;

			double p, cnr;

			p = std::sqrt(cn - 3000) / 64;
			cnr = (-1634.6 * p * p * p * p * p) + (14341 * p * p * p * p) - (50259 * p * p * p) + (88977 * p * p) - (89565 * p) + 58857;

			if (!std::isnan(cnr))
				value = static_cast<std::int32_t>(cnr);

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
