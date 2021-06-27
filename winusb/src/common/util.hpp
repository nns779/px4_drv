// util.hpp

#pragma once

#include <string>

#include <guiddef.h>

#include "type.hpp"

namespace px4 {

namespace util {

static std::uint32_t atoui(const char *str)
{
	std::uint32_t value = 0;

	for (; *str; str++) {
		if (*str < '0' || *str > '9')
			break;

		value *= 10;
		value += *str - '0';
	}

	return value;
}

bool HexFromStr(const wchar_t c, std::uint8_t &v);
bool HexStrToUInt8(const wchar_t *str, std::uint8_t &v);
bool ShiftJisToUtf16(const char *sjis, std::wstring &utf16);
bool ParseGuidStr(const std::wstring &utf16, GUID &guid);
bool ParseSystemStr(const std::wstring &str, px4::SystemType &systems);

namespace path {

bool Init(void *mod);
const std::wstring& GetDir() noexcept;
const std::wstring& GetFileBase() noexcept;

} // namespace path

} // namespace util

} // namespace px4
