#include "GLideN64_mupenplus.h"
#include <algorithm>
#include <string>
#include "../PluginAPI.h"
#include <Graphics/Context.h>
#include "../DisplayWindow.h"
#include <Graphics/OpenGLContext/GLFunctions.h>
#include <Graphics/OpenGLContext/opengl_Utils.h>
#include "../RSP.h"

#include <libretro_private.h>

extern retro_environment_t environ_cb;
extern uint32_t *rdram_size;

extern "C" void retroChangeWindow()
{
	dwnd().setToggleFullscreen();
	dwnd().changeWindow();
}

int PluginAPI::InitiateGFX(const GFX_INFO & _gfxInfo)
{
	_initiateGFX(_gfxInfo);

	REG.SP_STATUS = _gfxInfo.SP_STATUS_REG;
	rdram_size = (unsigned int*)_gfxInfo.RDRAM_SIZE;

	return TRUE;
}
