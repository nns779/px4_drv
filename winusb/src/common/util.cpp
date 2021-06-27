// util.cpp

#include "util.hpp"

#include <windows.h>

namespace px4 {

namespace util {

bool HexFromStr(const wchar_t c, std::uint8_t &v)
{
	if (c >= L'0' && c <= L'9')
		v = c - '0';
	else if (c >= 'A' && c <= L'F')
		v = c - 'A' + 0x0a;
	else if (c >= 'a' && c <= L'f')
		v = c - 'a' + 0x0a;
	else
		return false;

	return true;
}

bool HexStrToUInt8(const wchar_t *str, std::uint8_t &v)
{
	std::uint8_t t;

	if (!HexFromStr(str[0], t))
		return false;

	v = t << 4;

	if (!HexFromStr(str[1], t))
		return false;

	v |= t;

	return true;
}

bool ShiftJisToUtf16(const char *sjis, std::wstring &utf16)
{
	int required_size;

	required_size = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, sjis, -1, nullptr, 0);
	if (!required_size)
		return false;

	WCHAR *buf = new WCHAR[required_size];

	if (!MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, sjis, -1, buf, required_size))
		return false;

	utf16 = buf;
	delete[] buf;

	return true;
}

bool ParseGuidStr(const std::wstring &utf16, GUID &guid)
{
	GUID tmp;
	const wchar_t *str = utf16.c_str();
	wchar_t *end;

	if (std::wcslen(str) != 38 ||
		str[0] != L'{' ||
		str[9] != L'-' ||
		str[14] != L'-' ||
		str[19] != L'-' ||
		str[24] != L'-' ||
		str[37] != L'}')
		return false;

	tmp.Data1 = std::wcstoul(&str[1], &end, 16);
	if (end != &str[9])
		return false;

	tmp.Data2 = static_cast<unsigned short>(std::wcstoul(&str[10], &end, 16));
	if (end != &str[14])
		return false;

	tmp.Data3 = static_cast<unsigned short>(std::wcstoul(&str[15], &end, 16));
	if (end != &str[19])
		return false;

	for (int i = 0, j = 0; i < 17; i += 2, j++) {
		if (i == 4)
			i++;

		if (!HexStrToUInt8(&str[20 + i], tmp.Data4[j]))
			return false;
	}

	guid = tmp;

	return true;
}

bool ParseSystemStr(const std::wstring &str, px4::SystemType &systems)
{
	const wchar_t *head, *tail, *p, *split;

	systems = px4::SystemType::UNSPECIFIED;

	head = p = str.c_str();
	tail = head + std::wcslen(head);

	while (p <= tail) {
		p += std::wcsspn(p, L", \t");

		split = p + std::wcscspn(p, L", \t");
		if (split == p)
			split = tail;

		if (!std::wcsncmp(L"ISDB-T", p, 6))
			systems |= px4::SystemType::ISDB_T;
		else if (!std::wcsncmp(L"ISDB-S", p, 6))
			systems |= px4::SystemType::ISDB_S;
		else {
			systems = px4::SystemType::UNSPECIFIED;
			return false;
		}

		p = split + 1;
	}

	return true;
}

namespace path {

static std::wstring dir_path;
static std::wstring file_base;

bool Init(void *mod)
{
	WCHAR path[MAX_PATH];
	wchar_t *filename, *fileext;

	if (!GetModuleFileNameW(static_cast<HMODULE>(mod), path, MAX_PATH - 4))
		return false;

	filename = std::wcsrchr(path, L'\\');
	if (!filename)
		filename = std::wcsrchr(path, L'/');

	if (!filename)
		filename = path;
	else
		filename++;

	fileext = std::wcsrchr(filename, L'.');
	if (fileext)
		*fileext = L'\0';
	else
		fileext = filename + std::wcslen(filename);

	file_base = std::wstring(path);

	*filename = L'\0';
	dir_path = std::wstring(path);

	return true;
}

const std::wstring& GetDir() noexcept
{
	return dir_path;
}

const std::wstring& GetFileBase() noexcept
{
	return file_base;
}

} // namespace path

} // namespace util

} // namespace px4
