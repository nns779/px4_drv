// device_manager.cpp

#include "device_manager.hpp"

#include <algorithm>

#include <windows.h>
#include <setupapi.h>

#include "px4_device.hpp"
#include "pxmlt_device.hpp"

namespace px4 {

void DeviceManager::NotifyHandler::Handle(px4::DeviceNotifyType type, const GUID &interface_guid, const wchar_t *path) noexcept
{
	try {
		std::wstring path_lower = path;

		std::transform(path_lower.cbegin(), path_lower.cend(), path_lower.begin(), tolower);

		switch (type) {
		case px4::DeviceNotifyType::ARRIVAL:
			parent_.Add(path_lower, parent_.device_map_.at(interface_guid));
			break;

		case px4::DeviceNotifyType::REMOVE:
			parent_.Remove(path_lower);
			break;
		}
	} catch (const std::out_of_range &) {}
}

DeviceManager::DeviceManager(const px4::DeviceDefinitionSet &device_defs, px4::ReceiverManager &receiver_manager)
	: device_map_(),
	receiver_manager_(receiver_manager),
	mtx_(),
	index_(0),
	handler_(*this)
{
	auto &all_devs = device_defs.GetAll();

	for (auto it = all_devs.cbegin(); it != all_devs.cend(); ++it) {
		DeviceType type = DeviceType::UNKNOWN;
		
		if (it->first == L"PX4")
			type = DeviceType::PX4;
		else if (it->first == L"PXMLT")
			type = DeviceType::PXMLT;

		if (type == DeviceType::UNKNOWN)
			continue;

		auto &devs = it->second;

		for (auto it2 = devs.begin(); it2 != devs.end(); ++it2)
			device_map_.emplace(it2->device_interface_guid, std::move(std::pair<DeviceType, px4::DeviceDefinition>(type, *it2)));
	}

	notifier_.reset(new px4::DeviceNotifier(&handler_));

	for (auto it = device_map_.cbegin(); it != device_map_.cend(); ++it)
		Search(it->first, it->second);
}

DeviceManager::~DeviceManager()
{
	notifier_.reset();
}

void DeviceManager::Search(const GUID &guid, const std::pair<DeviceType, px4::DeviceDefinition> &def)
{
	HDEVINFO dev_info;

	dev_info = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
	if (dev_info == INVALID_HANDLE_VALUE)
		throw DeviceManagerError("px4::DeviceManager::Search: SetupDiGetClassDevsW() failed.");

	SP_DEVICE_INTERFACE_DATA intf_data;
	DWORD i = 0;

	intf_data.cbSize = sizeof(intf_data);

	while (SetupDiEnumDeviceInterfaces(dev_info, nullptr, &guid, i, &intf_data)) {
		DWORD detail_size;
		SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail_data;

		SetupDiGetDeviceInterfaceDetailW(dev_info, &intf_data, nullptr, 0, &detail_size, nullptr);
		if (!detail_size)
			continue;

		detail_data = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(new std::uint8_t[detail_size]);
		detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

		if (SetupDiGetDeviceInterfaceDetailW(dev_info, &intf_data, detail_data, detail_size, nullptr, nullptr)) {
			std::wstring path_lower = detail_data->DevicePath;

			std::transform(path_lower.cbegin(), path_lower.cend(), path_lower.begin(), tolower);
			Add(path_lower, def);
		}

		delete[] reinterpret_cast<std::uint8_t *>(detail_data);

		i++;
	}

	SetupDiDestroyDeviceInfoList(dev_info);
}


void DeviceManager::Add(const std::wstring &path, const std::pair<DeviceType, px4::DeviceDefinition> &def)
{
	std::lock_guard<std::mutex> lock(mtx_);

	if (Exists(path))
		return;

	switch (def.first) {
	case px4::DeviceType::PX4:
	{
		auto dev = std::make_unique<Px4Device>(path, def.second, ++index_, receiver_manager_);
		
		if (!dev->Init())
			devices_.emplace(path, std::move(dev));

		break;
	}

	case px4::DeviceType::PXMLT:
	{
		auto dev = std::make_unique<PxMltDevice>(path, def.second, ++index_, receiver_manager_);

		if (!dev->Init())
			devices_.emplace(path, std::move(dev));

		break;
	}

	default:
		break;
	}

	return;
}

void DeviceManager::Remove(const std::wstring &path)
{
	std::lock_guard<std::mutex> lock(mtx_);

	if (!Exists(path))
		return;

	devices_.at(path)->SetAvailability(false);
	devices_.erase(path);
}

bool DeviceManager::Exists(const std::wstring &path) const
{
	return !!devices_.count(path);
}

} // namespace px4
