#include "Log.h"
#include <string.h>
#include <android/log.h>
#include <vector>
#include <sstream>

void LogDebug(const char* f, int lin, int lvl, const char* fmt, ...)
{
	static android_LogPriority androidLogTranslate[] = {
			ANDROID_LOG_VERBOSE,
			ANDROID_LOG_INFO,
			ANDROID_LOG_WARN,
			ANDROID_LOG_ERROR
	};

	if (lvl > LOG_LEVEL)
		return;

	va_list va;
	va_start(va, fmt);
	__android_log_vprint(androidLogTranslate[lvl], "GLideN64", fmt, va);
	va_end(va);
}
