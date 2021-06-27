// receiver_info_set.cpp

#include "receiver_info_set.hpp"

#include "util.hpp"

namespace px4 {

void ReceiverInfoSet::Load(const px4::ConfigSet &configs) noexcept
{
	for (int i = 0; i <= 512; i++) {
		wchar_t sct[32];

		swprintf_s(sct, L"ReceiverDefinition%d", i);

		if (!configs.Exists(sct))
			break;

		const px4::Config &c = configs.Get(sct);
		const std::wstring &device_name = c.Get(L"DeviceName", L"");
		const std::wstring &device_guid_str = c.Get(L"DeviceGUID", L"");
		const std::wstring &receiver_name = c.Get(L"ReceiverName", L"");
		const std::wstring &receiver_guid_str = c.Get(L"ReceiverGUID", L"");
		const std::wstring &system_str = c.Get(L"System", L"");
		const std::wstring &index_str = c.Get(L"Index", L"-1");

		px4::command::ReceiverInfo receiver_info = { 0 };

		wcscpy_s(receiver_info.device_name, device_name.c_str());
		px4::util::ParseGuidStr(device_guid_str, receiver_info.device_guid);
		wcscpy_s(receiver_info.receiver_name, receiver_name.c_str());
		px4::util::ParseGuidStr(receiver_guid_str, receiver_info.receiver_guid);
		px4::util::ParseSystemStr(system_str, receiver_info.systems);
		receiver_info.index = static_cast<std::int32_t>(std::stol(index_str));
		receiver_info.data_id = 0;

		receivers_.push_back(receiver_info);
	}
}

const px4::command::ReceiverInfo& ReceiverInfoSet::Get(std::size_t i) const
{
	return receivers_.at(i);
}

} // namespace px4
