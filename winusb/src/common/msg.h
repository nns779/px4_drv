// msg.h

#pragma once

#include <stdbool.h>
#include <wctype.h>

enum msg_mode {
	MSG_MODE_NONE = 0x0000,
	MSG_MODE_CONSOLE = 0x0001,
	MSG_MODE_DEBUGGER = 0x0002,
	MSG_MODE_LOG_FILE = 0x0004
};

#ifdef __cplusplus
extern "C" {
#endif
void msg_set_mode(enum msg_mode new_mode);
bool msg_open_file(const wchar_t *path);
void msg_close_file();
int msg_printf(const char *format, ...);
#ifdef __cplusplus
}
#endif

#ifndef msg_prefix
#define msg_prefix	""
#endif

#define msg_err(format, ...)	msg_printf("[ERR] " msg_prefix ": " format, ##__VA_ARGS__)
#define msg_warn(format, ...)	msg_printf("[WARN] " msg_prefix ": " format, ##__VA_ARGS__)
#define msg_info(format, ...)	msg_printf("[INFO] " msg_prefix ": " format, ##__VA_ARGS__)
#if defined(_DEBUG) || defined(_DEBUG_MSG)
#define msg_dbg(format, ...)	msg_printf("[DBG] " msg_prefix ": " format, ##__VA_ARGS__)
#else
#define msg_dbg(format, ...)
#endif
