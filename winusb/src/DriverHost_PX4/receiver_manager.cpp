// receiver_manager.cpp

#include "receiver_manager.hpp"

#include <random>

namespace px4 {

bool ReceiverManager::Register(px4::command::ReceiverInfo &info, px4::ReceiverBase *receiver)
{
	std::lock_guard<std::shared_mutex> lock(mtx_);

	if (data_.count(receiver))
		return false;

	data_.emplace(receiver, ReceiverData{ info, false });
	return true;
}

bool ReceiverManager::Unregister(px4::ReceiverBase *receiver)
{
	std::lock_guard<std::shared_mutex> lock(mtx_);

	if (!data_.count(receiver))
		return false;

	data_.erase(receiver);
	return true;
}

static GUID empty_guid = { 0 };

px4::ReceiverBase* ReceiverManager::SearchAndOpen(px4::command::ReceiverInfo &key, px4::command::ReceiverInfo &info, std::uint32_t &data_id)
{
	std::lock_guard<std::shared_mutex> lock(mtx_);

	for (auto it = data_.cbegin(); it != data_.cend(); ++it) {
		const px4::command::ReceiverInfo& k = it->second.info;

		if (k.data_id)
			continue;

		if (key.device_name[0] && wcscmp(key.device_name, k.device_name))
			continue;

		if (memcmp(&key.device_guid, &empty_guid, sizeof(key.device_guid) && memcmp(&key.device_guid, &k.device_guid, sizeof(key.device_guid))))
			continue;

		if (key.receiver_name[0] && wcscmp(key.receiver_name, k.receiver_name))
			continue;

		if (memcmp(&key.receiver_guid, &empty_guid, sizeof(key.receiver_guid) && memcmp(&key.receiver_guid, &k.receiver_guid, sizeof(key.receiver_guid))))
			continue;

		if ((key.systems & k.systems) != key.systems)
			continue;

		if ((key.index >= 0) && (key.index != k.index))
			continue;

		px4::ReceiverBase *r = it->first;

		if (r->Open())
			continue;

		if (!GenerateDataId(r, data_id)) {
			r->Close();
			continue;
		}

		info = k;

		return r;
	}

	return nullptr;
}

px4::ReceiverBase* ReceiverManager::SearchByDataId(std::uint32_t data_id)
{
	std::shared_lock<std::shared_mutex> lock(mtx_);

	for (auto it = data_.begin(); it != data_.end(); ++it) {
		if (it->second.info.data_id != data_id || !it->second.valid_data_id)
			continue;

		it->second.valid_data_id = false;
		return it->first;
	}

	return nullptr;
}

bool ReceiverManager::GenerateDataId(px4::ReceiverBase *receiver, std::uint32_t &data_id)
{
	if (!data_.count(receiver))
		return false;

	auto& v = data_.at(receiver);

	if (v.info.data_id)
		return false;

	while (true) {
		std::uint32_t tmp = std::random_device()();

		if (!tmp)
			continue;

		auto it = data_.cbegin();
		for (; it != data_.cend(); ++it) {
			if (it->second.info.data_id == tmp)
				break;
		}

		if (it == data_.cend()) {
			v.info.data_id = data_id = tmp;
			v.valid_data_id = true;
			break;
		}
	}

	return true;
}

void ReceiverManager::ClearDataId(px4::ReceiverBase *receiver)
{
	std::lock_guard<std::shared_mutex> lock(mtx_);

	if (!data_.count(receiver))
		return;

	auto& data = data_.at(receiver);

	data.info.data_id = 0;
	data.valid_data_id = false;

	return;
}

} // namespace px4
