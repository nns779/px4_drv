// util.hpp

#pragma once

#include <string>

#include <guiddef.h>

#include "type.hpp"

namespace px4 {

namespace util {

static std::uint32_t atoui32(const char *str)
{
	std::uint32_t value = 0;

	while (*str == ' ' || *str == '\t')
		str++;

	for (; *str; str++) {
		if (*str < '0' || *str > '9')
			break;

		value *= 10;
		value += *str - '0';
	}

	return value;
}

static std::uint32_t wtoui32(const std::wstring &str)
{
	const wchar_t *p = str.c_str();
	std::uint32_t value = 0;

	while (*p == L' ' || *p == L'\t')
		p++;

	for (; *p; p++) {
		if (*p < L'0' || *p > L'9')
			break;

		value *= 10;
		value += *p - L'0';
	}

	return value;
}

static int wtoi(const std::wstring &str)
{
	const wchar_t *p = str.c_str();
	wchar_t sign = L'\0';
	int value = 0;

	while (*p == L' ' || *p == L'\t')
		p++;

	if (*p == L'+' || *p == L'-')
		sign = *p++;

	for (; *p; p++) {
		if (*p < L'0' || *p > L'9')
			break;

		value *= 10;
		value += *p - L'0';
	}

	return (sign == L'-') ? -value : value;
}

static unsigned int wtoui(const std::wstring &str)
{
	const wchar_t *p = str.c_str();
	unsigned int value = 0;

	while (*p == L' ' || *p == L'\t')
		p++;

	for (; *p; p++) {
		if (*p < L'0' || *p > L'9')
			break;

		value *= 10;
		value += *p - L'0';
	}

	return value;
}

static bool wtob(const std::wstring &str)
{
	return (str == L"true" || str == L"Y");
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
