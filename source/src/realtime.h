// Real time for the whole mod, immune to speed hacks.
//
// Speed hacks (Cheat Engine's speedhack and the like) scale GetTickCount / QueryPerformanceCounter / timeGetTime for
// the whole process - the mod's own timeouts would run N times too fast or too slow (host heartbeat, GPU waits, panel
// input, peak-hold timers). This header is force-included into every C++ file of the mod (scripts/build.sh) and maps
// those calls to the interrupt time in KUSER_SHARED_DATA (0x7FFE0008, 100 ns units), a page the kernel updates and no
// user-mode hook can change. The game itself keeps whatever speed the speed hack gives it.
#pragma once
#include <windows.h>
#include <stdint.h>

static inline uint64_t AC2VR_Interrupt100ns() {
    // KSYSTEM_TIME { ULONG LowPart; LONG High1Time; LONG High2Time; } at 0x7FFE0008
    volatile const uint32_t* t = (volatile const uint32_t*)(uintptr_t)0x7FFE0008;
    for (;;) {
        uint32_t hi1 = t[1];
        uint32_t lo = t[0];
        uint32_t hi2 = t[2];
        if (hi1 == hi2) return ((uint64_t)hi1 << 32) | lo;
    }
}
static inline DWORD AC2VR_RealTickCount() { return (DWORD)(AC2VR_Interrupt100ns() / 10000u); }
static inline BOOL AC2VR_RealQPC(LARGE_INTEGER* c) { c->QuadPart = (LONGLONG)AC2VR_Interrupt100ns(); return TRUE; }
static inline BOOL AC2VR_RealQPF(LARGE_INTEGER* f) { f->QuadPart = 10000000; return TRUE; }

#define GetTickCount AC2VR_RealTickCount
#define QueryPerformanceCounter AC2VR_RealQPC
#define QueryPerformanceFrequency AC2VR_RealQPF
