// SPDX-License-Identifier: GPL-2.0-only
#include "linux_card_device.hpp"

extern "C" {
#include <debuglog.h>
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace px4 {

LinuxCardDevice::~LinuxCardDevice()
{
	Disconnect();
}

int LinuxCardDevice::Connect(const std::string &path)
{
	if (fd_ >= 0)
		return -EALREADY;
	fd_ = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY);
	return fd_ < 0 ? -errno : 0;
}

void LinuxCardDevice::Disconnect() noexcept
{
	if (fd_ >= 0) {
		/* Linux の close() は EINTR でも記述子を解放済みなので、再試行すると別の記述子を閉じてしまう */
		close(fd_);
		fd_ = -1;
	}
}

int LinuxCardDevice::Request(unsigned long command, void *argument)
{
	if (fd_ < 0)
		return -ENODEV;
	/* カーネルモジュールは USB の操作を始める前にだけ EINTR を返すため、再実行しても操作は重複しない */
	int ret;
	do {
		ret = ioctl(fd_, command, argument);
	} while (ret < 0 && errno == EINTR);
	if (ret == 0)
		return 0;
	const int error = errno;
	if (error == ENOTTY)
		Log2(PCSC_LOG_ERROR, "ioctl(0x%lx) is not supported by the px4_drv kernel module", command);
	return -error;
}

int LinuxCardDevice::OpenCard()
{
	return Request(PX4_CARD_POWER_ON, nullptr);
}

int LinuxCardDevice::PowerOff()
{
	return Request(PX4_CARD_POWER_OFF, nullptr);
}

void LinuxCardDevice::CloseCard()
{
	PowerOff();
}

int LinuxCardDevice::DetectCard(bool &detected)
{
	__u32 value = 0;
	int ret = Request(PX4_CARD_DETECT, &value);
	detected = !ret && value != 0;
	return ret;
}

int LinuxCardDevice::ResetCard()
{
	return Request(PX4_CARD_RESET, nullptr);
}

int LinuxCardDevice::SetCardBaudrate(it930x_uart_baudrate baudrate)
{
	__u32 value;
	switch (baudrate) {
	case IT930X_UART_BAUDRATE_9600: value = 9600; break;
	case IT930X_UART_BAUDRATE_19200: value = 19200; break;
	case IT930X_UART_BAUDRATE_38400: value = 38400; break;
	default: return -EINVAL;
	}
	return Request(PX4_CARD_SET_BAUDRATE, &value);
}

int LinuxCardDevice::IsCardDataReady(bool &ready)
{
	__u32 value = 0;
	int ret = Request(PX4_CARD_RX_READY, &value);
	ready = !ret && value != 0;
	return ret;
}

int LinuxCardDevice::ReadCardData(std::uint8_t *buf, std::uint8_t &len)
{
	if (!buf || !len)
		return -EINVAL;
	px4_card_frame frame{};
	frame.length = len;
	int ret = Request(PX4_CARD_READ, &frame);
	if (ret)
		return ret;
	if (frame.length > len)
		return -EBADMSG;
	std::memcpy(buf, frame.data, frame.length);
	len = frame.length;
	return 0;
}

int LinuxCardDevice::WriteCardData(const std::uint8_t *buf, std::uint8_t len)
{
	if (!buf || !len)
		return -EINVAL;
	px4_card_frame frame{};
	frame.length = len;
	std::memcpy(frame.data, buf, len);
	return Request(PX4_CARD_WRITE, &frame);
}

static bool ReadNumber(const std::filesystem::path &path, unsigned int &value,
		       bool hex = false)
{
	std::ifstream stream(path);
	if (hex)
		stream >> std::hex;
	return bool(stream >> value);
}

/*
 * PX-Q3 系は2つの USB デバイスで1つの筐体を構成し、カーネルモジュールはシリアル番号の末尾の桁が 1 の側にだけカードリーダーを登録する
 * Windows 版の Px4Device::HasCardReader() と同じ判定で、カードリーダーが現れない側を待たずに済ませる
 */
static bool LacksCardSlot(unsigned int vendor, unsigned int product,
			  unsigned int bus, unsigned int device, const std::string &usbsys)
{
	if (vendor != 0x0511 || (product != 0x084a && product != 0x024a && product != 0x074a))
		return false;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(usbsys, ec)) {
		unsigned int b, d;
		if (!ReadNumber(entry.path() / "busnum", b) || !ReadNumber(entry.path() / "devnum", d) ||
		    b != bus || d != device)
			continue;
		std::string serial;
		std::ifstream(entry.path() / "serial") >> serial;
		return serial.size() == 15 && serial.back() != '1';
	}
	return false;
}

int ResolveCardDevice(const std::string &name, std::string &path,
		      const std::string &sysfs, const std::string &devdir,
		      const std::string &usbsys)
{
	if (!name.empty() && name[0] == '/') {
		path = name;
		return 0;
	}
	unsigned int vendor, product, interface, bus, device;
	int end = 0;
	if (std::sscanf(name.c_str(), "usb:%x/%x:libudev:%u:/dev/bus/usb/%u/%u%n",
		&vendor, &product, &interface, &bus, &device, &end) != 5 ||
	    end != static_cast<int>(name.size()) || interface != 0)
		return -EINVAL;

	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(sysfs, ec)) {
		const auto parent = std::filesystem::canonical(entry.path() / "device", ec);
		if (ec)
			continue;
		const auto usb = parent.parent_path();
		unsigned int v, p, b, d;
		if (ReadNumber(usb / "idVendor", v, true) &&
		    ReadNumber(usb / "idProduct", p, true) &&
		    ReadNumber(usb / "busnum", b) && ReadNumber(usb / "devnum", d) &&
		    v == vendor && p == product && b == bus && d == device) {
			/* デバイスファイルの置き場所はカーネルの devnode で決まるため、パスはカーネルが公開する DEVNAME から取る */
			std::ifstream uevent(entry.path() / "uevent");
			std::string line;
			while (std::getline(uevent, line)) {
				if (line.compare(0, 8, "DEVNAME=") != 0)
					continue;
				const std::filesystem::path devname(line.substr(8));
				if (devname.empty() || devname.is_absolute())
					return -EINVAL;
				for (const auto &component : devname) {
					if (component == "..")
						return -EINVAL;
				}
				path = (std::filesystem::path(devdir) / devname).string();
				return 0;
			}
		}
	}
	/* 登録を待つ ENODEV と区別し、カードリーダーを持たない側は ENXIO ですぐに失敗させる */
	return LacksCardSlot(vendor, product, bus, device, usbsys) ? -ENXIO : -ENODEV;
}

} // namespace px4
