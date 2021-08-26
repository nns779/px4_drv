// device_definition_set.hpp

#pragma once

#include <cstdint>

#include <string>
#include <functional>
#include <vector>
#include <unordered_map>

#include <guiddef.h>

#include "type.hpp"
#include "util.hpp"
#include "config.hpp"

namespace px4 {

struct ReceiverDefinition final {
	std::wstring name;
	GUID guid;
	px4::SystemType systems;
	std::int32_t index;
};

struct DeviceDefinition final {
	std::wstring name;
	GUID guid;
	GUID device_interface_guid;
	Config configs;
	std::vector<ReceiverDefinition> receivers;
};

class DeviceDefinitionSet final {
public:
	DeviceDefinitionSet() noexcept {}
	~DeviceDefinitionSet() {};

	void Load(const px4::ConfigSet &configs) noexcept;
	const std::vector<DeviceDefinition>& Get(const std::wstring &type) const;
	const std::unordered_map<std::wstring, std::vector<DeviceDefinition>>& GetAll() const;

private:
	std::unordered_map<std::wstring, std::vector<DeviceDefinition>> devices_;
};

} // namespace px4
