// device_manager.hpp

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <stdexcept>

#include <guiddef.h>

#include "device_notifier.hpp"
#include "device_definition_set.hpp"
#include "device_base.hpp"
#include "receiver_manager.hpp"

namespace px4 {

enum class DeviceType : std::uint32_t {
	UNKNOWN = 0,
	PX4,
	PXMLT
};

class DeviceManager final {
private:
	class NotifyHandler final : public px4::DeviceNotifyHandler {
	public:
		explicit NotifyHandler(DeviceManager &parent) : parent_(parent) {}
		~NotifyHandler() {}

		void Handle(px4::DeviceNotifyType type, const GUID &interface_guid, const wchar_t *path) noexcept;

	private:
		DeviceManager &parent_;
	};

public:
	explicit DeviceManager(const px4::DeviceDefinitionSet &device_defs, px4::ReceiverManager &receiver_manager);
	~DeviceManager();

	// cannot copy
	DeviceManager(const DeviceManager &) = delete;
	DeviceManager& operator=(const DeviceManager &) = delete;

	// cannot move
	DeviceManager(DeviceManager &&) = delete;
	DeviceManager& operator=(DeviceManager &&) = delete;

private:
	void Search(const GUID &guid, const std::pair<DeviceType, px4::DeviceDefinition> &def);
	void Add(const std::wstring &path, const std::pair<DeviceType, px4::DeviceDefinition> &def);
	void Remove(const std::wstring &path);
	bool Exists(const std::wstring &path) const;

	std::unordered_map<GUID, std::pair<DeviceType, px4::DeviceDefinition>> device_map_;
	px4::ReceiverManager &receiver_manager_;

	std::mutex mtx_;
	std::unordered_map<std::wstring, std::unique_ptr<DeviceBase>> devices_;
	std::uintptr_t index_;
	NotifyHandler handler_;
	std::unique_ptr<px4::DeviceNotifier> notifier_;
};

class DeviceManagerError : public std::runtime_error {
public:
	explicit DeviceManagerError(const std::string &what_arg) : runtime_error(what_arg.c_str()) {};
};

} // namespace px4
