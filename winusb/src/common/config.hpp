// config.hpp

#pragma once

#include <string>
#include <unordered_map>
#include <stdexcept>

namespace px4 {

class Config final {
public:
	Config() noexcept {}
	~Config() {}

	bool Load(const std::wstring &path, const std::wstring &section) noexcept;
	bool Exists(const std::wstring &key) const noexcept;
	const std::wstring& Get(const std::wstring &key) const;
	const std::wstring Get(const std::wstring &key, const std::wstring &default_value) const;

private:
	std::unordered_map<std::wstring, std::wstring> values_;
};

class ConfigSet final {
public:
	ConfigSet() noexcept {}
	~ConfigSet() {}

	bool Load(const std::wstring &path) noexcept;
	bool Exists(const std::wstring &sct) const noexcept;
	const px4::Config& Get(const std::wstring &sct) const;

private:
	std::unordered_map<std::wstring, Config> configs_;
};

} // namespace px4
