#include "GLideN64_mupenplus.h"
#include <algorithm>
#include <string>
#include <sstream>
#include <iostream>
#include <Platform.h>
#include "../PluginAPI.h"
#include "../RSP.h"

#if defined(OS_MAC_OS_X)
#include <mach-o/dyld.h>
#endif

int PluginAPI::InitiateGFX(const GFX_INFO & _gfxInfo)
{
	_initiateGFX(_gfxInfo);

	int core_version;
	unsigned int gfx_info_version = 1;
	CoreGetVersion(NULL, &core_version, NULL, NULL, NULL);
	if (core_version >= 0x020501)
		gfx_info_version = _gfxInfo.version;
	if (gfx_info_version >= 2) {
		REG.SP_STATUS = _gfxInfo.SP_STATUS_REG;
		rdram_size = _gfxInfo.RDRAM_SIZE;
	}

	return TRUE;
}
