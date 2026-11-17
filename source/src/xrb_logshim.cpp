// xrbridge client logging -> da2vr.log
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include "log.h"
namespace xrb {
static int g_level = 2;
void LogInit(const wchar_t*, int level) { g_level = level; }
void LogSetLevel(int level) { g_level = level; }
int LogGetLevel() { return g_level; }
void LogFlush() {}
void LogWrite(int level, const char* fmt, ...) {
    if (level > g_level) return;
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    ::LogWrite(level == 0 ? "E" : level == 1 ? "W" : "I", "%s", buf);
}
}
