// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <string>
#include "card_device.hpp"
#include "px4_card_ioctl.h"

namespace px4 {

class LinuxCardDevice final : public CardDevice {
public:
	~LinuxCardDevice() override;
	int Connect(const std::string &path);
	void Disconnect() noexcept;
	int OpenCard() override;
	void CloseCard() override;
	int PowerOff();
	int DetectCard(bool &detected) override;
	int ResetCard() override;
	int SetCardBaudrate(std::uint32_t baudrate) override;
	int IsCardDataReady(bool &ready) override;
	int ReadCardData(std::uint8_t *buf, std::uint8_t &len) override;
	int WriteCardData(const std::uint8_t *buf, std::uint8_t len) override;

private:
	int Request(unsigned long command, void *argument);
	int fd_ = -1;
};

/* 同じ機種を複数台つないだときに VID と PID だけでは区別できないため、sysfs で USB のバス番号とデバイス番号まで照合する */
int ResolveCardDevice(const std::string &name, std::string &path,
		      const std::string &sysfs = "/sys/class/px4card",
		      const std::string &devdir = "/dev",
		      const std::string &usbsys = "/sys/bus/usb/devices");

} // namespace px4
