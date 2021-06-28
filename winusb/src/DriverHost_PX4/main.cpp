// main.cpp

#define msg_prefix	"DriverHost_PX4"

#include <string>
#include <iostream>
#include <stdexcept>

#include <windows.h>

#include "driver_host.hpp"
#include "util.hpp"
#include "msg.h"

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR pCmdLine, _In_ int nCmdShow)
{
#ifdef _DEBUG
	FILE *fp;

	AllocConsole();

	freopen_s(&fp, "CONOUT$", "w", stdout);
	freopen_s(&fp, "CONOUT$", "w", stderr);

	msg_set_mode(MSG_MODE_CONSOLE);
#else
	msg_set_mode(MSG_MODE_DEBUGGER);
#endif

	msg_info("Start\n");

	px4::util::path::Init(hInstance);

	SetCurrentDirectoryW(px4::util::path::GetDir().c_str());

	try {
		px4::DriverHost host;

		host.Run();
	} catch (const std::runtime_error & e) {
		msg_err("%s\n", e.what());
		MessageBoxA(nullptr, e.what(), "DriverHost_PX4 (wWinMain)", MB_OK | MB_ICONERROR);
	}

	msg_info("Exiting...\n");

#ifdef _DEBUG
	Sleep(2000);
	FreeConsole();
#endif

	return 0;
}
