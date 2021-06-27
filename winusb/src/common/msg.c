// msg.c

#include "msg.h"

#include <stdarg.h>
#include <stdio.h>

#include <windows.h>

static enum msg_mode mode = MSG_MODE_NONE;
static HANDLE file_handle = INVALID_HANDLE_VALUE;

void msg_set_mode(enum msg_mode new_mode)
{
	mode = new_mode;
}

bool msg_open_file(const wchar_t *path)
{
	if (file_handle != INVALID_HANDLE_VALUE)
		CloseHandle(file_handle);

	file_handle = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	return (file_handle != INVALID_HANDLE_VALUE) ? true : false;
}

void msg_close_file()
{
	if (file_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(file_handle);
		file_handle = INVALID_HANDLE_VALUE;
	}

	return;
}

int msg_printf(const char *format, ...)
{
	va_list args;
	char buf[1024];
	int c;

	if (mode == MSG_MODE_NONE)
		return 0;

	va_start(args, format);
	c = vsprintf_s(buf, 1024, format, args);
	va_end(args);
	
	if (mode & MSG_MODE_CONSOLE)
		printf("%s", buf);

	if (mode & MSG_MODE_DEBUGGER)
		OutputDebugStringA(buf);

	if ((mode & MSG_MODE_LOG_FILE) && (file_handle != INVALID_HANDLE_VALUE)) {
		DWORD wb;
		
		WriteFile(file_handle, buf, c, &wb, NULL);
	}

	return c;
}
