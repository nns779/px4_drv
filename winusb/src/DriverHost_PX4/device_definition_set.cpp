// device_definition_set.cpp

#include "device_definition_set.hpp"

namespace px4 {

void DeviceDefinitionSet::Load(const px4::ConfigSet &configs) noexcept
{
	for (int i = 0; i < 512; i++) {
		wchar_t dev_sct[32];

		swprintf_s(dev_sct, L"DeviceDefinition%d", i);

		if (!configs.Exists(dev_sct))
			break;

		const px4::Config &cd = configs.Get(dev_sct);

		const std::wstring &dev_name = cd.Get(L"Name", L"");
		const std::wstring &dev_guid_str = cd.Get(L"GUID", L"");
		const std::wstring &dev_type = cd.Get(L"Type", L"PX4");
		const std::wstring &dev_intf_guid_str = cd.Get(L"DeviceInterfaceGUID", L"");

		DeviceDefinition dev_def = {};

		dev_def.name = dev_name;
		px4::util::ParseGuidStr(dev_guid_str, dev_def.guid);
		px4::util::ParseGuidStr(dev_intf_guid_str, dev_def.device_interface_guid);

		for (int j = 0; j < 64; j++) {
			wchar_t rcvr_sct[64];

			swprintf_s(rcvr_sct, L"DeviceDefinition%d.Receiver%d", i, j);

			if (!configs.Exists(rcvr_sct))
				break;

			const px4::Config &cr = configs.Get(rcvr_sct);

			const std::wstring &rcvr_name = cr.Get(L"Name", L"");
			const std::wstring &rcvr_guid_str = cr.Get(L"GUID", L"");
			const std::wstring &rcvr_system_str = cr.Get(L"System", L"");
			const std::wstring &rcvr_index_str = cr.Get(L"Index", L"-1");

			ReceiverDefinition rcvr_def = {};

			rcvr_def.name = rcvr_name;
			px4::util::ParseGuidStr(rcvr_guid_str, rcvr_def.guid);
			px4::util::ParseSystemStr(rcvr_system_str, rcvr_def.systems);
			rcvr_def.index = static_cast<std::int32_t>(std::stol(rcvr_index_str));

			dev_def.receivers.emplace_back(rcvr_def);
		}

		if (!devices_.count(dev_type))
			devices_.emplace(dev_type, std::vector<px4::DeviceDefinition>());

		devices_.at(dev_type).emplace_back(dev_def);
	}
}

const std::vector<DeviceDefinition>& DeviceDefinitionSet::Get(const std::wstring &type) const
{
	return devices_.at(type);
}

const std::unordered_map<std::wstring, std::vector<DeviceDefinition>>& DeviceDefinitionSet::GetAll() const
{
	return devices_;
}

} // namespace px4
