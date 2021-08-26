// config.cpp

#include "config.hpp"

#include <memory>
#include <iostream>

#include <windows.h>

namespace px4 {

bool Config::Load(const std::wstring &path, const std::wstring &section) noexcept
{
	std::unique_ptr<WCHAR[]> data(new WCHAR[16384]);
	DWORD len;

	values_.clear();

	len = GetPrivateProfileSectionW(section.c_str(), data.get(), 16384, path.c_str());
	if (!len)
		return false;

	WCHAR *p = data.get();

	while (*p != L'\0') {
		wchar_t *split, *key, *value;
		std::size_t value_len;

		split = std::wcschr(p, L'=');
		if (split == nullptr)
			continue;

		*split = L'\0';
		key = p;
		value = split + 1;
		value_len = std::wcslen(value);

		if ((value[0] == L'\"' && value[value_len - 1] == L'\"') ||
			(value[0] == L'\'' && value[value_len - 1] == L'\'')) {
			value[value_len - 1] = L'\0';
			value++;
			value_len--;
		}

		values_.emplace(key, value);

		p = value + value_len + 1;
	}

	return true;
}

bool Config::Exists(const std::wstring &key) const noexcept
{
	return !!values_.count(key);
}

const std::wstring& Config::Get(const std::wstring &key) const
{
	return values_.at(key);
}

const std::wstring Config::Get(const std::wstring &key, const std::wstring &default_value) const
{
	try {
		return Get(key);
	} catch (const std::out_of_range&) {
		return default_value;
	}
}

bool ConfigSet::Load(const std::wstring &path) noexcept
{
	std::unique_ptr<WCHAR[]> sct(new WCHAR[8192]);
	DWORD len;

	configs_.clear();

	len = GetPrivateProfileSectionNamesW(sct.get(), 8192, path.c_str());
	if (!len)
		return false;

	for (WCHAR *p = sct.get(); *p != L'\0'; p += std::wcslen(p) + 1) {
		auto v = configs_.emplace(p, Config());

		if (!v.first->second.Load(path, p)) {
			configs_.clear();
			return false;
		}
	}

	return true;
}

bool ConfigSet::Exists(const std::wstring &sct) const noexcept
{
	return !!configs_.count(sct);
}

const px4::Config& ConfigSet::Get(const std::wstring &sct) const
{
	return configs_.at(sct);
}

} // namespace px4
