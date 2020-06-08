#ifdef OS_WINDOWS
# include <windows.h>
#else
# include "winlnxdefs.h"
#endif // OS_WINDOWS
#include "RSP.h"
#include "PluginAPI.h"
#include "Config.h"
#include "GBI.h"
#include "wst.h"

void Config::resetToDefaults()
{
	version = CONFIG_VERSION_CURRENT;

	video.fxaa = 0;
	video.multisampling = 0;
	video.threadedVideo = 0;

	texture.maxAnisotropy = 0;
	texture.bilinearMode = BILINEAR_STANDARD;
	texture.enableHalosRemoval = 0;

	generalEmulation.enableLOD = 1;
	generalEmulation.enableNoise = 1;
	generalEmulation.enableHWLighting = 0;
	generalEmulation.enableShadersStorage = 1;
	generalEmulation.enableLegacyBlending = 0;
	generalEmulation.enableHybridFilter = 1;
	generalEmulation.hacks = 0;
#if defined(OS_ANDROID) || defined(OS_IOS)
	generalEmulation.enableFragmentDepthWrite = 0;
	generalEmulation.enableBlitScreenWorkaround = 0;
	generalEmulation.forcePolygonOffset = 0;
	generalEmulation.polygonOffsetFactor = 0.0f;
	generalEmulation.polygonOffsetUnits = 0.0f;
#else
	generalEmulation.enableFragmentDepthWrite = 1;
#endif

	graphics2D.correctTexrectCoords = tcDisable;
	graphics2D.enableNativeResTexrects = NativeResTexrectsMode::ntDisable;
	graphics2D.bgMode = BGMode::bgOnePiece;

	frameBufferEmulation.enable = 1;
	frameBufferEmulation.copyDepthToRDRAM = cdSoftwareRender;
	frameBufferEmulation.copyFromRDRAM = 0;
	frameBufferEmulation.copyAuxToRDRAM = 0;
	frameBufferEmulation.copyToRDRAM = ctDoubleBuffer;
	frameBufferEmulation.N64DepthCompare = dcDisable;
	frameBufferEmulation.forceDepthBufferClear = 0;
	frameBufferEmulation.aspect = a43;
	frameBufferEmulation.bufferSwapMode = bsOnVerticalInterrupt;
	frameBufferEmulation.nativeResFactor = 0;
	frameBufferEmulation.fbInfoReadColorChunk = 0;
	frameBufferEmulation.fbInfoReadDepthChunk = 1;
	frameBufferEmulation.copyDepthToMainDepthBuffer = 0;
#ifndef MUPENPLUSAPI
	frameBufferEmulation.fbInfoDisabled = 0;
#else
	frameBufferEmulation.fbInfoDisabled = 1;
#endif

	textureFilter.txFilterMode = 0;
	textureFilter.txEnhancementMode = 0;
	textureFilter.txDeposterize = 0;
	textureFilter.txFilterIgnoreBG = 0;
	textureFilter.txCacheSize = 100 * gc_uMegabyte;

	textureFilter.txHiresEnable = 0;
	textureFilter.txHiresFullAlphaChannel = 1;
	textureFilter.txHresAltCRC = 0;

	textureFilter.txForce16bpp = 0;

	/* FIXME: Chang to buffer. */
	gln_wcscat(textureFilter.txPath, wst("hires_texture"));
	gln_wcscat(textureFilter.txCachePath, wst("cache"));

	gammaCorrection.force = 0;
	gammaCorrection.level = 2.0f;

	debug.dumpMode = 0;
}

bool isHWLightingAllowed()
{
	if (config.generalEmulation.enableHWLighting == 0)
		return false;
	return GBI.isHWLSupported();
}

void Config::validate()
{
	if (frameBufferEmulation.enable != 0 && frameBufferEmulation.N64DepthCompare != dcDisable)
		video.multisampling = 0;
	if (frameBufferEmulation.nativeResFactor == 1) {
		graphics2D.enableNativeResTexrects = 0;
		graphics2D.correctTexrectCoords = tcDisable;
	} else {
		if (graphics2D.enableNativeResTexrects != 0)
			graphics2D.correctTexrectCoords = tcDisable;
	}
}
