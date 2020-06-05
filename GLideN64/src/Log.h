#pragma once

#define LOG_DEBUG	-1
#define LOG_APIFUNC	LOG_DEBUG
#define LOG_VERBOSE	0
#define LOG_INFO	1
#define LOG_WARNING	2
#define LOG_ERROR  	3

#ifdef __cplusplus
extern "C" {
#endif
	void LogDebug(const char* f, int lin, int lvl, const char* fmt, ... );
#ifdef __cplusplus
};
#endif

#ifdef NDEBUG
#define LOG(lvl, fmt...) LogDebug(__func__, __LINE__, lvl, fmt)
#else
#define LOG(lvl, fmt...) LogDebug(__FILE__, __LINE__, lvl, fmt)
#endif
