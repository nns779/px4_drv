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

namespace px4 {

class DeviceBase {
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

protected:
	const device& GetDevice() const { return dev_; }

	const px4::DeviceDefinition &device_def_;
	px4::ReceiverManager &receiver_manager_;

	device dev_;
	usb_device usb_dev_;
};

class DeviceError : public std::runtime_error {
public:
	explicit DeviceError(const std::string &what_arg) : runtime_error(what_arg.c_str()) {};
};

} // namespace px4
