// device_base.cpp

#include "device_base.hpp"

#include <cinttypes>

#include "msg.h"
#include "command.hpp"

namespace px4 {

DeviceBase::DeviceBase(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager)
	: device_def_(device_def),
	receiver_manager_(receiver_manager)
{
	strncpy_s(dev_.driver_name, "px4_winusb", sizeof("px4_winusb"));
	sprintf_s(dev_.device_name, "%" PRIuPTR, index);

	usb_dev_.winusb = nullptr;

	usb_dev_.dev = CreateFileW(
		path.c_str(),
		GENERIC_WRITE | GENERIC_READ,
		FILE_SHARE_WRITE | FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
		nullptr);
	if (usb_dev_.dev == INVALID_HANDLE_VALUE)
		throw DeviceError("px4::DeviceBase::DeviceBase: CreateFileW() failed.");

	if (!WinUsb_Initialize(usb_dev_.dev, &usb_dev_.winusb)) {
		CloseHandle(usb_dev_.dev);
		throw DeviceError("px4::DeviceBase::DeviceBase: WinUsb_Initialize() failed.");
	}

	ULONG size;

	if (!WinUsb_GetDescriptor(
		usb_dev_.winusb,
		USB_DEVICE_DESCRIPTOR_TYPE,
		0x00,
		0x0000,
		reinterpret_cast<PUCHAR>(&usb_dev_.descriptor),
		sizeof(usb_dev_.descriptor),
		&size)
	) {
		WinUsb_Free(usb_dev_.winusb);
		CloseHandle(usb_dev_.dev);
		throw DeviceError("px4::DeviceBase::DeviceBase: WinUsb_GetDescriptor(USB_DEVICE_DESCRIPTOR_TYPE) failed.");
	}
}

DeviceBase::~DeviceBase()
{
	if (usb_dev_.winusb) {
		WinUsb_Free(usb_dev_.winusb);
		usb_dev_.winusb = nullptr;
	}

	if (usb_dev_.dev != INVALID_HANDLE_VALUE) {
		CloseHandle(usb_dev_.dev);
		usb_dev_.dev = INVALID_HANDLE_VALUE;
	}
}

} // namespace px4
