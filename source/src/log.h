#pragma once
// Mod log next to the game exe. Lines are buffered in memory and written by a background thread (log.cpp).
#include <cstdio>
#include <cstdarg>

void LogInit(const wchar_t* path);
void LogWrite(const char* level, const char* fmt, ...);
void LogFlushDisk();   // background thread: write pending lines (commit to disk every 5 s)
void LogFlushNow();    // crash handler: write and commit now

#define LOGI(...) LogWrite("I", __VA_ARGS__)
#define LOGW(...) LogWrite("W", __VA_ARGS__)
#define LOGE(...) LogWrite("E", __VA_ARGS__)
