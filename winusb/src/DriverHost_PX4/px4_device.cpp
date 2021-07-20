// px4_device.cpp

#include "px4_device.hpp"

#include <cassert>
#include <cmath>

#include "type.hpp"
#include "command.hpp"
#include "misc_win.h"

namespace px4 {

struct Px4MultiDeviceModeParam final {
	Px4MultiDeviceMode mode;
	const wchar_t str[8];
} px4_mldev_mode_param[] = {
	{ Px4MultiDeviceMode::ALL, L"all" },
	{ Px4MultiDeviceMode::S_ONLY, L"s-only" },
	{ Px4MultiDeviceMode::S0_ONLY, L"s0-only" },
	{ Px4MultiDeviceMode::S1_ONLY, L"s1-only" },
};

Px4Device::Px4Device(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager)
	: DeviceBase(path, device_def, index, receiver_manager),
	config_(),
	lock_(),
	available_(true),
	serial_(),
	init_(false),
	open_count_(0),
	lnb_power_count_(0),
	streaming_count_(0)
{
	LoadConfig();
	ParseSerialNumber();

	memset(&it930x_, 0, sizeof(it930x_));
	memset(&stream_ctx_, 0, sizeof(stream_ctx_));
}

Px4Device::~Px4Device()
{
	Term();
}

void Px4Device::LoadConfig()
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

	if (configs.Exists(L"DisableMultiDevicePowerControl"))
		config_.device.disable_multi_device_power_control = px4::util::wtob(configs.Get(L"DisableMultiDevicePowerControl"));

	if (configs.Exists(L"MultiDevicePowerControlMode")) {
		auto &mode_str = configs.Get(L"MultiDevicePowerControlMode");

		for (int i = 0; i < sizeof(px4_mldev_mode_param) / sizeof(px4_mldev_mode_param[0]); i++) {
			if (mode_str == px4_mldev_mode_param[i].str) {
				config_.device.multi_device_power_control_mode = px4_mldev_mode_param[i].mode;
				break;
			}
		}
	}

	if (configs.Exists(L"DiscardNullPackets"))
		config_.device.discard_null_packets = px4::util::wtob(configs.Get(L"DiscardNullPackets"));

	dev_dbg(&dev_, "px4::Px4Device::LoadConfig: xfer_packets: %u\n", config_.usb.xfer_packets);
	dev_dbg(&dev_, "px4::Px4Device::LoadConfig: urb_max_packets: %u\n", config_.usb.urb_max_packets);
	dev_dbg(&dev_, "px4::Px4Device::LoadConfig: max_urbs: %u\n", config_.usb.max_urbs);
	dev_dbg(&dev_, "px4::Px4Device::LoadConfig: no_raw_io: %s\n", (config_.usb.no_raw_io) ? "true" : "false");
	dev_dbg(&dev_, "px4::Px4Device::LoadConfig: receiver_max_packets: %u\n", config_.device.receiver_max_packets);
	dev_dbg(&dev_, "px4::Px4Device::LoadConfig: psb_purge_timeout: %i\n", config_.device.psb_purge_timeout);
	dev_dbg(&dev_, "px4::Px4Device::LoadConfig: disable_multi_device_power_control: %s\n", (config_.device.disable_multi_device_power_control) ? "true" : "false");
	dev_dbg(&dev_, "px4::Px4Device::LoadConfig: discard_null_packets: %s\n", (config_.device.discard_null_packets) ? "true" : "false");

	return;
}

void Px4Device::ParseSerialNumber() noexcept
{
	if (!usb_dev_.serial)
		return;

	try {
		serial_.serial_number = std::stoull(usb_dev_.serial->bString);
		serial_.dev_id = static_cast<std::uint8_t>(serial_.serial_number % 10);
		serial_.serial_number /= 10;

		dev_dbg(&dev_, "px4::Px4Device::ParseSerialNumber: serial_number: %014llu\n", serial_.serial_number);
		dev_dbg(&dev_, "px4::Px4Device::ParseSerialNumber: dev_id: %u\n", serial_.dev_id);
	} catch (...) {}

	return;
}

int Px4Device::Init()
{
	dev_dbg(&dev_, "px4::Px4Device::Init: init_: %s\n", (init_) ? "true" : "false");

	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (init_)
		return -EALREADY;

	int ret = 0;
	bool use_mldev = false;
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

	it930x_.config.input[0].i2c_addr = 0x11;
	it930x_.config.input[1].i2c_addr = 0x13;
	it930x_.config.input[2].i2c_addr = 0x10;
	it930x_.config.input[3].i2c_addr = 0x12;

	it930x_.config.input[4].enable = false;
	it930x_.config.input[4].port_number = 0;

	for (int i = 0; i < 4; i++) {
		it930x_stream_input &input = it930x_.config.input[i];

		input.enable = true;
		input.is_parallel = false;
		input.port_number = i + 1;
		input.slave_number = i;
		input.i2c_bus = 2;
		input.packet_len = 188;
		input.sync_byte = ((i + 1) << 4) | 0x07;

		receivers_[i].reset(new Px4Receiver(*this, i));
		stream_ctx_.stream_buf[i] = receivers_[i]->GetStreamBuffer();
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

	ret = it930x_set_gpio_mode(&it930x_, 2, IT930X_GPIO_OUT, true);
	if (ret)
		goto fail_device;

	switch (usb_dev_.descriptor.idProduct) {
	case 0x084a:
	case 0x024a:
	case 0x074a:
		use_mldev = !config_.device.disable_multi_device_power_control;
		break;

	default:
		break;
	}

	if (use_mldev) {
		if (MultiDevice::Search(serial_.serial_number, mldev_))
			ret = mldev_->Add(*this);
		else
			ret = MultiDevice::Alloc(*this, config_.device.multi_device_power_control_mode, mldev_);

		if (ret)
			goto fail_device;
	} else {
		ret = it930x_write_gpio(&it930x_, 7, true);
		if (ret)
			goto fail_device;

		ret = it930x_write_gpio(&it930x_, 2, false);
		if (ret)
			goto fail_device;
	}

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

		for (int i = 0; i < 4; i++) {
			ret = it930x_set_pid_filter(&it930x_, i, &filter);
			if (ret)
				goto fail_device;
		}
	}

	init_ = true;

	for (auto it = device_def_.receivers.cbegin(); it != device_def_.receivers.cend(); ++it) {
		px4::command::ReceiverInfo ri;

		if ((it->systems != px4::SystemType::ISDB_T && it->systems != px4::SystemType::ISDB_S) || (it->index < 0 || it->index > 2))
			continue;

		wcscpy_s(ri.device_name, device_def_.name.c_str());
		ri.device_guid = device_def_.guid;
		wcscpy_s(ri.receiver_name, it->name.c_str());
		ri.receiver_guid = it->guid;
		ri.systems = it->systems;
		ri.index = it->index;
		ri.data_id = 0;

		receiver_manager_.Register(ri, receivers_[((it->systems == px4::SystemType::ISDB_T) ? 2 : 0) + it->index].get());
	}

	return 0;

fail_device:
	for (int i = 0; i < 4; i++) {
		stream_ctx_.stream_buf[i] = nullptr;
		receivers_[i].reset();
	}

	it930x_term(&it930x_);

fail_bridge:
	itedtv_bus_term(&bus);

fail_bus:
	return ret;

}

void Px4Device::Term()
{
	dev_dbg(&dev_, "px4::Px4Device::Term: init_: %s\n", (init_) ? "true" : "false");

	std::unique_lock<std::recursive_mutex> lock(lock_);

	if (!init_)
		return;

	for (int i = 0; i < 4; i++)
		receiver_manager_.Unregister(receivers_[i].get());

	lock.unlock();

	for (int i = 0; i < 4; i++) {
		stream_ctx_.stream_buf[i] = nullptr;
		receivers_[i].reset();
	}

	lock.lock();

	it930x_term(&it930x_);
	itedtv_bus_term(&it930x_.bus);

	init_ = false;
	return;
}

void Px4Device::SetAvailability(bool available)
{
	available_ = available;
}

ReceiverBase* Px4Device::GetReceiver(int id) const
{
	if (id < 0 || id >= 4)
		throw std::out_of_range("receiver id out of range");

	return receivers_[id].get();
}

const i2c_comm_master& Px4Device::GetI2cMaster(int bus) const
{
	if (bus < 1 || bus > 3)
		throw std::out_of_range("bus number out of range");

	return it930x_.i2c_master[bus - 1];
}

int Px4Device::SetBackendPower(bool state)
{
	dev_dbg(&dev_, "px4::Px4Device::SetBackendPower: %s\n", (state) ? "true" : "false");

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

int Px4Device::SetLnbVoltage(std::int32_t voltage)
{
	dev_dbg(&dev_, "px4::Px4Device::SetBackendPower: voltage: %d\n", voltage);

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

int Px4Device::PrepareCapture()
{
	dev_dbg(&dev_, "px4::Px4Device::PrepareCapture\n");

	int ret = 0;
	std::lock_guard<std::recursive_mutex> lock(lock_);

	if (streaming_count_)
		return 0;

	ret = it930x_purge_psb(&it930x_, config_.device.psb_purge_timeout);
	if (ret)
		dev_err(&dev_, "px4::Px4Device::PrepareCapture: it930x_purge_psb() failed. (ret: %d)\n", ret);

	return (ret && ret != -ETIMEDOUT) ? ret : 0;
}

int Px4Device::StartCapture()
{
	dev_dbg(&dev_, "px4::Px4Device::StartCapture\n");

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
			dev_err(&dev_, "px4::Px4Device::StartCapture: itedtv_bus_start_streaming() failed. (ret: %d)\n", ret);
			return ret;
		}
	}

	streaming_count_++;
	dev_dbg(&dev_, "px4::Px4Device::StartCapture: streaming_count_: %u\n", streaming_count_);

	return 0;
}

int Px4Device::StopCapture()
{
	dev_dbg(&dev_, "px4::Px4Device::StopCapture\n");

	std::lock_guard<std::recursive_mutex> lock(lock_);

	assert(streaming_count_ > 0);

	streaming_count_--;
	if (!streaming_count_) {
		dev_dbg(&dev_, "px4::Px4Device::StopCapture: stopping...\n");
		itedtv_bus_stop_streaming(&it930x_.bus);
	} else {
		dev_dbg(&dev_, "px4::Px4Device::StopCapture: streaming_count_: %u\n", streaming_count_);
	}

	return 0;
}

void Px4Device::StreamProcess(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf[], std::uint8_t **buf, std::size_t &len)
{
	std::uint8_t *p = *buf;
	std::size_t remain = len;

	while (remain) {
		std::size_t i;
		bool sync_remain = false;

		for (i = 0; i < PX4_DEVICE_TS_SYNC_COUNT; i++) {
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

		if (i < PX4_DEVICE_TS_SYNC_COUNT) {
			p++;
			remain--;
			continue;
		}

		while (remain >= 188 && ((p[0] & 0x8f) == 0x07)) {
			u8 id = (p[0] & 0x70) >> 4;

			if (id && id < 5) {
				std::size_t pkt_len = 188;

				p[0] = 0x47;
				stream_buf[id - 1]->Write(p, pkt_len);
			}

			p += 188;
			remain -= 188;
		}
	}

	for (int i = 0; i < 4; i++)
		stream_buf[i]->NotifyWrite();

	*buf = p;
	len = remain;

	return;
}

int Px4Device::StreamHandler(void *context, void *buf, std::uint32_t len)
{
	Px4Device &obj = *static_cast<Px4Device*>(context);
	StreamContext &stream_ctx = obj.stream_ctx_;
	std::uint8_t *p = static_cast<std::uint8_t*>(buf);
	std::size_t remain = len;

	if (stream_ctx.remain_len) {
		if ((stream_ctx.remain_len + len) >= PX4_DEVICE_TS_SYNC_SIZE) {
			std::uint8_t * remain_buf = stream_ctx.remain_buf;
			std::size_t t = PX4_DEVICE_TS_SYNC_SIZE - stream_ctx.remain_len;

			memcpy(remain_buf + stream_ctx.remain_len, p, t);
			stream_ctx.remain_len = PX4_DEVICE_TS_SYNC_SIZE;

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

std::mutex Px4Device::MultiDevice::mldev_list_lock_;
std::unordered_map<std::uint64_t, std::shared_ptr<Px4Device::MultiDevice>> Px4Device::MultiDevice::mldev_list_;

Px4Device::MultiDevice::MultiDevice(Px4MultiDeviceMode mode, std::uint64_t serial_number)
	: lock_(),
	serial_number_(serial_number),
	mode_(mode),
	dev_{ nullptr, nullptr },
	power_state_{ false, false },
	receiver_state_{ { false, false, false, false }, { false, false, false, false } }
{

}

Px4Device::MultiDevice::~MultiDevice()
{

}

bool Px4Device::MultiDevice::Search(std::uint64_t serial_number, std::shared_ptr<MultiDevice> &mldev)
{
	msg_dbg("px4::Px4Device::MultiDevice::Search\n");

	try {
		std::lock_guard<std::mutex> lock(mldev_list_lock_);

		mldev = mldev_list_.at(serial_number);
		return true;
	} catch (std::out_of_range &) {
		return false;
	}
}

int Px4Device::MultiDevice::Alloc(Px4Device &dev, Px4MultiDeviceMode mode, std::shared_ptr<MultiDevice> &mldev)
{
	msg_dbg("px4::Px4Device::MultiDevice::Alloc\n");

	std::uint8_t dev_id = dev.serial_.dev_id - 1;

	msg_dbg("px4::Px4Device::MultiDevice::Alloc: serial_number: %014llu, dev_id: %u\n", dev.serial_.serial_number, dev.serial_.dev_id);

	if (dev_id > 1)
		return -EINVAL;

	{
		std::lock_guard<std::mutex> lock(mldev_list_lock_);
		mldev = mldev_list_.emplace(dev.serial_.serial_number, new MultiDevice(mode, dev.serial_.serial_number)).first->second;
	}

	mldev->dev_[dev_id] = &dev;
	return 0;
}

int Px4Device::MultiDevice::Add(Px4Device &dev)
{
	msg_dbg("px4::Px4Device::MultiDevice::Add\n");

	std::uint8_t dev_id = dev.serial_.dev_id - 1;
	
	if (dev_id > 1)
		return -EINVAL;

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	if (GetDeviceCount() == 2)
		return -EINVAL;

	if (dev_[dev_id])
		return -EALREADY;

	power_state_[dev_id] = false;
	for (int i = 0; i < 4; i++)
		receiver_state_[dev_id][i] = false;

	if (IsPowerIntelockingRequried((dev_id) ? 0 : 1)) {
		ret = dev.SetBackendPower(true);
		if (ret)
			return ret;

		power_state_[dev_id] = true;
	}

	dev_[dev_id] = &dev;
	return 0;
}

int Px4Device::MultiDevice::Remove(Px4Device &dev)
{
	msg_dbg("px4::Px4Device::MultiDevice::Remove\n");

	std::uint8_t dev_id = dev.serial_.dev_id - 1;
	std::uint8_t other_dev_id = (dev_id) ? 1 : 0;

	if (dev_id > 1)
		return -EINVAL;

	std::lock_guard<std::mutex> lock(lock_);

	if (dev_[dev_id] != &dev)
		return -EINVAL;

	if (power_state_[dev_id])
		dev.SetBackendPower(false);

	dev_[dev_id] = nullptr;
	power_state_[dev_id] = false;
	for (int i = 0; i < 4; i++)
		receiver_state_[dev_id][i] = false;

	if (dev_[other_dev_id] && !GetReceiverStatus(other_dev_id) && power_state_[other_dev_id]) {
		dev_[other_dev_id]->SetBackendPower(false);
		power_state_[other_dev_id] = false;
	}

	if (!GetDeviceCount()) {
		std::lock_guard<std::mutex> mldev_list_lock(mldev_list_lock_);
		mldev_list_.erase(serial_number_);
	}

	return 0;
}

std::uint8_t Px4Device::MultiDevice::GetDeviceCount() const noexcept
{
	return (!!dev_[0]) + (!!dev_[1]);
}

bool Px4Device::MultiDevice::GetReceiverStatus(std::uint8_t dev_id) const noexcept
{
	auto &state = receiver_state_[dev_id];
	return (state[0] || state[1] || state[2] || state[3]);
}

bool Px4Device::MultiDevice::IsPowerIntelockingRequried(std::uint8_t dev_id) const noexcept
{
	bool ret = false;
	auto &state = receiver_state_[dev_id];

	switch (mode_) {
	case Px4MultiDeviceMode::S_ONLY:
		ret = state[0] || state[1];
		break;

	case Px4MultiDeviceMode::S0_ONLY:
		ret = state[0];
		break;

	case Px4MultiDeviceMode::S1_ONLY:
		ret = state[1];
		break;

	default:
		ret = state[0] || state[1] || state[2] || state[3];
		break;
	}

	msg_dbg("px4::Px4Device::MultiDevice::IsPowerInterlockingRequired: ret: %s\n", (ret) ? "true" : "false");

	return ret;
}

int Px4Device::MultiDevice::SetPower(Px4Device &dev, std::uintptr_t index, bool state, bool *first)
{
	std::uint8_t dev_id = dev.serial_.dev_id - 1;
	std::uint8_t other_dev_id = (dev_id) ? 0 : 1;

	if (dev_id > 1 || index > 3)
		return -EINVAL;

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	if (dev_[dev_id] != &dev)
		return -EINVAL;

	if (receiver_state_[dev_id][index] == state)
		return 0;

	if (!state)
		receiver_state_[dev_id][index] = false;

	if (!GetReceiverStatus(dev_id)) {
		if (power_state_[dev_id] != state && (state || !IsPowerIntelockingRequried(other_dev_id))) {
			ret = dev.SetBackendPower(state);
			if (ret && state)
				return ret;

			power_state_[dev_id] = state;
		}

		if (state && first)
			*first = true;
	}

	if (state)
		receiver_state_[dev_id][index] = true;

	if (dev_[other_dev_id]) {
		bool interlocking = IsPowerIntelockingRequried(dev_id);

		if (interlocking == state && power_state_[other_dev_id] != interlocking && (state || !GetReceiverStatus(other_dev_id))) {
			ret = dev_[other_dev_id]->SetBackendPower(state);
			if (ret && state)
				return ret;

			power_state_[other_dev_id] = state;
		}
	}

	return 0;
}

struct tc90522_regbuf Px4Device::Px4Receiver::tc_init_s0_[] = {
	{ 0x07, NULL, { 0x31 } },
	{ 0x08, NULL, { 0x77 } }
};

struct tc90522_regbuf Px4Device::Px4Receiver::tc_init_t0_[] = {
	{ 0x0e, NULL, { 0x77 } },
	{ 0x0f, NULL, { 0x13 } }
};

struct tc90522_regbuf Px4Device::Px4Receiver::tc_init_t_[] = {
	{ 0xb0, NULL, { 0xa0 } },
	{ 0xb2, NULL, { 0x3d } },
	{ 0xb3, NULL, { 0x25 } },
	{ 0xb4, NULL, { 0x8b } },
	{ 0xb5, NULL, { 0x4b } },
	{ 0xb6, NULL, { 0x3f } },
	{ 0xb7, NULL, { 0xff } },
	{ 0xb8, NULL, { 0xc0 } },
	{ 0x1f, NULL, { 0x00 } },
	{ 0x75, NULL, { 0x00 } }
};

struct tc90522_regbuf Px4Device::Px4Receiver::tc_init_s_[] = {
	{ 0x15, NULL, { 0x00 } },
	{ 0x1d, NULL, { 0x00 } },
	{ 0x04, NULL, { 0x02 } }
};

Px4Device::Px4Receiver::Px4Receiver(Px4Device &parent, std::uintptr_t index)
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

	if (index == 0 || index == 1)
		system_ = px4::SystemType::ISDB_S;
	else if (index == 2 || index == 3)
		system_ = px4::SystemType::ISDB_T;
	else
		system_ = px4::SystemType::UNSPECIFIED;

	std::uint8_t demod_addr = parent_.it930x_.config.input[index].i2c_addr;
	const device *dev = &parent_.GetDevice();
	const i2c_comm_master *i2c = &parent_.GetI2cMaster(parent_.it930x_.config.input[index].i2c_bus);

	tc90522_.dev = dev;
	tc90522_.i2c = i2c;
	tc90522_.i2c_addr = demod_addr;
	tc90522_.is_secondary = (demod_addr & 0x0e) ? true : false;

	switch (system_) {
	case px4::SystemType::ISDB_T:
		r850_.dev = dev;
		r850_.i2c = &tc90522_.i2c_master;
		r850_.i2c_addr = 0x7c;
		r850_.config.xtal = 24000;
		r850_.config.loop_through = !tc90522_.is_secondary;
		r850_.config.clock_out = false;
		r850_.config.no_imr_calibration = true;
		r850_.config.no_lpf_calibration = true;
		break;

	case px4::SystemType::ISDB_S:
		rt710_.dev = dev;
		rt710_.i2c = &tc90522_.i2c_master;
		rt710_.i2c_addr = 0x7a;
		rt710_.config.xtal = 24000;
		rt710_.config.loop_through = false;
		rt710_.config.clock_out = false;
		rt710_.config.signal_output_mode = RT710_SIGNAL_OUTPUT_DIFFERENTIAL;
		rt710_.config.agc_mode = RT710_AGC_POSITIVE;
		rt710_.config.vga_atten_mode = RT710_VGA_ATTEN_OFF;
		rt710_.config.fine_gain = RT710_FINE_GAIN_3DB;
		rt710_.config.scan_mode = RT710_SCAN_MANUAL;
		break;
	}
}

Px4Device::Px4Receiver::~Px4Receiver()
{
	dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::~Px4Receiver(%u)\n", index_);

	std::unique_lock<std::mutex> lock(lock_);

	close_cond_.wait(lock, [this] { return !open_; });

	dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::~Px4Receiver(%u): exit\n", index_);
}

int Px4Device::Px4Receiver::Init(bool sleep)
{
	dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::Init(%u): init_: %s, sleep: %s\n", index_, (init_) ? "true" : "false", (sleep) ? "true" : "false");

	int ret = 0;
	std::lock_guard<std::recursive_mutex> dev_lock(parent_.lock_);

	if (init_)
		return 0;

	ret = tc90522_init(&tc90522_);
	if (ret) {
		dev_err(&parent_.dev_, "px4::Px4Device::Px4Receiver::Init(%u): tc90522_init() failed. (ret: %d)\n", index_, ret);
		return ret;
	}

	switch (system_) {
	case px4::SystemType::ISDB_T:
		ret = r850_init(&r850_);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Px4Device::Px4Receiver::Init(%u): r850_init() failed. (ret: %d)\n", index_, ret);
			break;
		}

		if (sleep) {
			ret = r850_sleep(&r850_);
			if (ret)
				break;

			ret = tc90522_sleep_t(&tc90522_, true);
			if (ret)
				break;
		}

		break;

	case px4::SystemType::ISDB_S:
		ret = rt710_init(&rt710_);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Px4Device::Px4Receiver::Init(%u): rt710_init() failed. (ret: %d)\n", index_, ret);
			break;
		}

		if (sleep) {
			ret = rt710_sleep(&rt710_);
			if (ret)
				break;

			ret = tc90522_sleep_s(&tc90522_, true);
			if (ret)
				break;
		}

		break;

	default:
		ret = -EINVAL;
		break;
	}

	if (!ret)
		init_.store(true);

	return ret;
}

void Px4Device::Px4Receiver::Term()
{
	dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::Term(%u): init_: %s\n", index_, (init_) ? "true" : "false");

	std::lock_guard<std::recursive_mutex> dev_lock(parent_.lock_);

	if (!init_)
		return;

	switch (system_) {
	case px4::SystemType::ISDB_T:
		r850_term(&r850_);
		break;

	case px4::SystemType::ISDB_S:
		rt710_term(&rt710_);
		break;

	default:
		break;
	}

	tc90522_term(&tc90522_);

	init_ = false;

	return;
}

int Px4Device::Px4Receiver::InitPrimary()
{
	std::lock_guard<std::recursive_mutex> dev_lock(parent_.lock_);

	if (tc90522_.is_secondary)
		return 0;

	int ret = 0;

	switch (system_) {
	case px4::SystemType::ISDB_T:
		ret = tc90522_write_multiple_regs(&tc90522_, tc_init_t0_, ARRAY_SIZE(tc_init_t0_));
		break;

	case px4::SystemType::ISDB_S:
		ret = tc90522_write_multiple_regs(&tc90522_, tc_init_s0_, ARRAY_SIZE(tc_init_s0_));
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

int Px4Device::Px4Receiver::Open()
{
	dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::Open(%u): init_: %s, open_: %s\n", index_, (init_) ? "true" : "false", (open_) ? "true" : "false");

	int ret = 0;
	std::unique_lock<std::mutex> lock(lock_);
	std::unique_lock<std::recursive_mutex> dev_lock(parent_.lock_);
	bool need_init = false;

	if (open_)
		return (!init_) ? -EINVAL : -EALREADY;

	if (parent_.mldev_) {
		ret = parent_.mldev_->SetPower(parent_, index_, true, &need_init);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Px4Device::Px4Receiver::Open(%u): mldev_->SetPower(true) failed. (ret: %d)\n", index_, ret);
			return ret;
		}
	} else if (!parent_.open_count_) {
		ret = parent_.SetBackendPower(true);
		if (ret) {
			dev_err(&parent_.dev_, "px4::Px4Device::Px4Receiver::Open(%u): parent_.SetBackendPower(true) failed. (ret: %d)\n", index_, ret);
			return ret;
		}
		need_init = true;
	}

	if (need_init) {
		for (int i = 0; i < 4; i++) {
			ret = parent_.receivers_[i]->Init((parent_.receivers_[i].get() == this) ? false : true);
			if (ret)
				break;
		}
		if (ret)
			goto fail;
	}	

	switch (system_) {
	case px4::SystemType::ISDB_T:
		ret = tc90522_write_multiple_regs(&tc90522_, tc_init_t_, ARRAY_SIZE(tc_init_t_));
		if (ret)
			break;

		ret = tc90522_enable_ts_pins_t(&tc90522_, false);
		if (ret)
			break;

		ret = tc90522_sleep_t(&tc90522_, false);
		if (ret)
			break;

		ret = r850_wakeup(&r850_);
		if (ret)
			break;

		r850_system_config sys;

		sys.system = R850_SYSTEM_ISDB_T;
		sys.bandwidth = R850_BANDWIDTH_6M;
		sys.if_freq = 4063;

		ret = r850_set_system(&r850_, &sys);
		if (ret)
			break;

		break;

	case px4::SystemType::ISDB_S:
		ret = tc90522_write_multiple_regs(&tc90522_, tc_init_s_, ARRAY_SIZE(tc_init_s_));
		if (ret)
			break;

		ret = tc90522_enable_ts_pins_s(&tc90522_, false);
		if (ret)
			break;

		ret = tc90522_sleep_s(&tc90522_, false);
		if (ret)
			break;

		break;

	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		goto fail;
	
	if (!parent_.open_count_) {
		for (int i = 0; i < 4; i += 2) {
			ret = parent_.receivers_[i]->InitPrimary();
			if (ret)
				break;
		}
		if (ret)
			goto fail;
	}

	parent_.open_count_++;

	open_ = true;
	return ret;

fail:
	if (parent_.mldev_)
		parent_.mldev_->SetPower(parent_, index_, false, nullptr);
	else if (!parent_.open_count_) 
		parent_.SetBackendPower(false);

	return ret;
}

void Px4Device::Px4Receiver::Close()
{
	dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::Close(%u): init_: %s, open_: %s\n", index_, (init_) ? "true" : "false", (open_) ? "true" : "false");

	if (!init_ || !open_)
		return;

	SetCapture(false);
	SetLnbVoltage(0);

	std::unique_lock<std::mutex> lock(lock_);
	std::lock_guard<std::recursive_mutex> dev_lock(parent_.lock_);

	assert(parent_.open_count_ > 0);

	parent_.open_count_--;
	if (!parent_.open_count_) {
		for (int i = 0; i < 4; i++) {
			if (parent_.receivers_[i])
				parent_.receivers_[i]->Term();
		}
		
		if (!parent_.mldev_)
			parent_.SetBackendPower(false);
	} else if (parent_.available_) {
		switch (system_) {
		case px4::SystemType::ISDB_T:
			r850_sleep(&r850_);
			tc90522_sleep_t(&tc90522_, true);
			break;

		case px4::SystemType::ISDB_S:
			rt710_sleep(&rt710_);
			tc90522_sleep_s(&tc90522_, true);
			break;
		}
	}

	if (parent_.mldev_)
		parent_.mldev_->SetPower(parent_, index_, false, nullptr);

	open_ = false;

	lock.unlock();
	close_cond_.notify_all();

	return;
}

int Px4Device::Px4Receiver::SetFrequency()
{
	if (!init_ || !open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::SetFrequency(%u): freq: %u\n", index_, params_.freq);

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	switch (system_) {
	case px4::SystemType::ISDB_T:
	{
		ret = tc90522_write_reg(&tc90522_, 0x47, 0x30);
		if (ret)
			break;

		ret = tc90522_set_agc_t(&tc90522_, false);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_, 0x76, 0x0c);
		if (ret)
			break;

		ret = r850_set_frequency(&r850_, params_.freq);
		if (ret)
			break;

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
			dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::SetFrequency(%u): tuner is NOT locked.\n", index_);
			ret = -EAGAIN;
			break;
		}

		ret = tc90522_set_agc_t(&tc90522_, true);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_, 0x71, 0x21);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_, 0x72, 0x25);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_, 0x75, 0x08);
		if (ret)
			break;

		break;
	}

	case px4::SystemType::ISDB_S:
	{
		ret = tc90522_set_agc_s(&tc90522_, false);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_, 0x8e, 0x06/*0x02*/);
		if (ret)
			break;

		ret = tc90522_write_reg(&tc90522_, 0xa3, 0xf7);
		if (ret)
			break;

		ret = rt710_set_params(&rt710_, params_.freq, 28860, 4);
		if (ret)
			break;

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
			dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::SetFrequency(%u): tuner is NOT locked.\n", index_);
			ret = -EAGAIN;
			break;
		}

		ret = tc90522_set_agc_s(&tc90522_, true);
		if (ret)
			break;

		break;
	}

	default:
		ret = -EINVAL;
		break;
	}

	dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::SetFrequency(%u): %s.\n", index_, (!ret) ? "succeeded" : "failed");

	return ret;
}

int Px4Device::Px4Receiver::CheckLock(bool &locked)
{
	if (!init_ || !open_)
		return -EINVAL;

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	switch (system_) {
	case px4::SystemType::ISDB_T:
		ret = tc90522_is_signal_locked_t(&tc90522_, &locked);
		break;

	case px4::SystemType::ISDB_S:
		ret = tc90522_is_signal_locked_s(&tc90522_, &locked);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

int Px4Device::Px4Receiver::SetStreamId()
{
	if (system_ != px4::SystemType::ISDB_S)
		return -EINVAL;

	if (!init_ || !open_)
		return -EINVAL;

	int ret = 0;
	std::uint16_t tsid;
	std::lock_guard<std::mutex> lock(lock_);

	if (params_.stream_id < 8) {
		for (int i = 50; i; i--) {
			ret = tc90522_tmcc_get_tsid_s(&tc90522_, params_.stream_id, &tsid);
			if ((!ret && tsid) || (ret == -EINVAL))
				break;

			Sleep(20);
		}

		if (ret)
			return ret;

		dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::SetStreamId(%u): slot: %u, tsid: 0x%04x\n", index_, params_.stream_id, tsid);
	} else {
		tsid = params_.stream_id;
	}

	ret = tc90522_set_tsid_s(&tc90522_, tsid);
	if (ret)
		return ret;

	int i;

	for (i = 50; i; i--) {
		std::uint16_t tsid2;

		ret = tc90522_get_tsid_s(&tc90522_, &tsid2);
		if (!ret && tsid2 == tsid)
			break;

		Sleep(20);
	}

	if (!ret && !i)
		ret = -EAGAIN;

	return ret;
}

int Px4Device::Px4Receiver::SetLnbVoltage(std::int32_t voltage)
{
	if (system_ != px4::SystemType::ISDB_S)
		return -EINVAL;

	if (!init_ || !open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::SetLnbVoltage(%u): voltage: %d\n", index_, voltage);

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

int Px4Device::Px4Receiver::SetCapture(bool capture)
{
	if (!init_ || !open_)
		return -EINVAL;

	dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::SetCapture(%u): capture: %s\n", index_, (capture) ? "true" : "false");

	if ((capture && streaming_) || (!capture && !streaming_))
		return -EALREADY;

	int ret = 0;
	std::lock_guard<std::mutex> lock(lock_);

	if (capture) {
		ret = parent_.PrepareCapture();
		if (ret)
			return ret;
	} else {
		ret = parent_.StopCapture();
		if (ret)
			return ret;

		stream_buf_->Stop();
		streaming_ = false;
	}

	switch (system_) {
	case px4::SystemType::ISDB_T:
		ret = tc90522_enable_ts_pins_t(&tc90522_, capture);
		if (ret)
			dev_err(&parent_.dev_, "px4::Px4Device::Px4Receiver::SetCapture(%u): tc90522_enable_ts_pins_t() failed.\n", index_);

		break;

	case px4::SystemType::ISDB_S:
		ret = tc90522_enable_ts_pins_s(&tc90522_, capture);
		if (ret)
			dev_err(&parent_.dev_, "px4::Px4Device::Px4Receiver::SetCapture(%u): tc90522_enable_ts_pins_s() failed.\n", index_);

		break;

	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		return ret;

	if (capture) {
		ret = parent_.StartCapture();
		if (ret)
			return ret;

		std::size_t size = 188 * parent_.config_.device.receiver_max_packets;

		stream_buf_->Alloc(size);
		stream_buf_->SetThresholdSize(size / 10);
		stream_buf_->Start();

		streaming_ = true;
	}

	dev_dbg(&parent_.dev_, "px4::Px4Device::Px4Receiver::SetCapture(%u): succeeded.\n", index_);

	return 0;
}

int Px4Device::Px4Receiver::ReadStat(px4::command::StatType type, std::int32_t &value)
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
		switch (system_) {
		case px4::SystemType::ISDB_T:
		{
			std::uint32_t cndat;

			ret = tc90522_get_cndat_t(&tc90522_, &cndat);
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

			ret = tc90522_get_cn_s(&tc90522_, &cn);
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
