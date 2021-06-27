// dllmain.cpp

#include <windows.h>

#include "util.hpp"

BOOL WINAPI DllMain(HANDLE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	BOOL ret = TRUE;
	
	switch (fdwReason) {
	case DLL_PROCESS_ATTACH:
		if (!px4::util::path::Init(hinstDLL))
			ret = FALSE;
		break;

	default:
		break;
	}
	
	return ret;
}
