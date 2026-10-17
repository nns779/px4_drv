// device_base.hpp

#pragma once

#include <cstdint>
#include <string>
#include <stdexcept>

#include <windows.h>
#include <winusb.h>

#include "type.hpp"
#include "command.hpp"
#include "device_definition_set.hpp"
#include "receiver_base.hpp"
#include "receiver_manager.hpp"

#include "misc_win.h"
#include "winusb_compat.h"
#include "it930x.h"
#include "card_device.hpp"

namespace px4 {

class DeviceBase : public CardDevice {
public:
	explicit DeviceBase(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager);
	virtual ~DeviceBase();

	// cannot copy
	DeviceBase(const DeviceBase &) = delete;
	DeviceBase& operator=(const DeviceBase &) = delete;

	// cannot move
	DeviceBase(DeviceBase &&) = delete;
	DeviceBase& operator=(DeviceBase &&) = delete;

	virtual int Init() = 0;
	virtual void Term() = 0;
	virtual void SetAvailability(bool available) = 0;
	virtual px4::ReceiverBase* GetReceiver(int id) const = 0;
	virtual bool HasCardReader() const noexcept { return false; }
	int OpenCard() override { return -ENOSYS; }
	void CloseCard() override {}
	int DetectCard(bool &detected) override { return -ENOSYS; }
	int ResetCard() override { return -ENOSYS; }
	int SetCardBaudrate(std::uint32_t baudrate) override { return -ENOSYS; }
	int IsCardDataReady(bool &ready) override { return -ENOSYS; }
	int ReadCardData(std::uint8_t *buf, std::uint8_t &len) override { return -ENOSYS; }
	int WriteCardData(const std::uint8_t *buf, std::uint8_t len) override { return -ENOSYS; }

	const std::wstring& GetCardReaderName() const noexcept { return card_reader_name_; }

protected:
	const device& GetDevice() const { return dev_; }

	const px4::DeviceDefinition &device_def_;
	px4::ReceiverManager &receiver_manager_;

	device dev_;
	usb_device usb_dev_;
	std::wstring usb_serial_number_;
	std::wstring card_reader_name_;
};

class DeviceError : public std::runtime_error {
public:
	explicit DeviceError(const std::string &what_arg) : runtime_error(what_arg.c_str()) {};
};

} // namespace px4
