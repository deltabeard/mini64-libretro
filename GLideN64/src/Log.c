#include <stdarg.h>
#include <stdio.h>

#include "Log.h"

#ifdef MUPENPLUSAPI
void DebugMessage(int level, const char *message, ...);
#endif

void LogDebug(const char* f, int lin, int lvl, const char* fmt, ...)
{
	char buf[256];
	int ret;

#ifdef NDEBUG
	if(lvl < LOG_WARNING)
		return;
#endif

	va_list va;
	va_start(va, fmt);
	ret = vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);

	if(ret < 0)
		return;

#ifdef MUPENPLUSAPI
	DebugMessage(lvl, "GLideN64 [%s:%d]: %s", f, lin, buf);
#else
	lvl++;
	const char *const lvl_str[] = { "DEBUG", "VERB", "INFO", "WARN", "ERROR" };
	fprintf(stderr, "%s: GLideN64 [%s:%d]: %s", lvl_str(lvl), f, lin, buf);
#endif
	return;
}
