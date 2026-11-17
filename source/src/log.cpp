// Log writer that never blocks the game's threads on the disk.
//
// LogWrite only formats the line and copies it into a memory buffer under a short lock. A background thread
// (LogFlushDisk, every 200 ms from main.cpp) swaps the buffer out and does the file write; FlushFileBuffers (a real
// disk commit) runs on that thread only, at most every 5 s, outside the lock. Before 0.7.1 every LogWrite did an
// fflush and the commit held the same lock, so a slow disk / antivirus could stall the render thread for hundreds of ms.
#include "log.h"
#include <windows.h>
#include <io.h>
#include <string>

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_cs, g_fileCs;
static LARGE_INTEGER g_freq, g_t0;
static std::string g_buf, g_out;      // pending lines; the writer's swap buffer
static unsigned g_dropped = 0;
static const size_t kMaxPending = 1 << 20;

void LogInit(const wchar_t* path) {
    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_fileCs);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_buf.reserve(64 * 1024);
    g_log = _wfopen(path, L"w");
}

void LogWrite(const char* level, const char* fmt, ...) {
    if (!g_log) return;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    double s = double(t.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
    char msg[2048], line[2200];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    int n = snprintf(line, sizeof line, "%9.3f [%s] [%5lu] %s\n", s, level, GetCurrentThreadId(), msg);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    EnterCriticalSection(&g_cs);
    if (g_buf.size() + n < kMaxPending) g_buf.append(line, n); else g_dropped++;
    LeaveCriticalSection(&g_cs);
}

// writes what is pending (any thread; the game's threads never call it except the crash handler)
static void Drain(bool commit) {
    if (!g_log) return;
    EnterCriticalSection(&g_fileCs);
    EnterCriticalSection(&g_cs);
    g_out.swap(g_buf); g_buf.clear();
    unsigned dropped = g_dropped; g_dropped = 0;
    LeaveCriticalSection(&g_cs);
    if (!g_out.empty()) { fwrite(g_out.data(), 1, g_out.size(), g_log); g_out.clear(); }
    if (dropped) fprintf(g_log, "          [W] log: %u lines dropped (too many at once)\n", dropped);
    fflush(g_log);
    if (commit) FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(g_log)));
    LeaveCriticalSection(&g_fileCs);
}

void LogFlushDisk() {
    static DWORD lastCommit = 0;
    DWORD now = GetTickCount();
    bool commit = now - lastCommit >= 5000;
    if (commit) lastCommit = now;
    Drain(commit);
}
void LogFlushNow() { Drain(true); }
