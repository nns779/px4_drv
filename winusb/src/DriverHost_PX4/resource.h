// resource.h

#pragma once

#include <windows.h>

#define VER_FILE	0,4,0,0
#define VER_FILE_STR	"0.4.0"

#define VER_PRODUCT	0,4,0,0
#define VER_PRODUCT_STR	"0.4.0"

#define VER_COMMENTS_STR		""
#define VER_COMPANYNAME_STR		"nns779"
#define VER_FILEDESCRIPTION_STR		"PX4 Device Driver (WinUSB)"
#define VER_INTERNALNAME_STR		"DriverHost_PX4.exe"
#define VER_LEGALCOPYRIGHT_STR		""
#define VER_LEGALTRADEMARKS_STR		""
#define VER_ORIGINALFILENAME_STR	VER_INTERNALNAME_STR
#define VER_PRIVATEBUILD_STR		""
#define VER_PRODUCTNAME_STR		"DriverHost_PX4"
#define VER_SPECIALBUILD_STR		""

#if defined(_DEBUG)
#define VER_FLAGS	VS_FF_PRERELEASE | VS_FF_DEBUG
#elif defined(_DEBUG_MSG)
#define VER_FLAGS	VS_FF_PRERELEASE
#else
#define VER_FLAGS	0
#endif
