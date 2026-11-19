#pragma once
/* Minimal POSIX unistd.h + clock_gettime shim for Porffor-generated C on Windows. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <setjmp.h>
#include <stdio.h>
#include <time.h>

#ifndef getcwd
#define getcwd _getcwd
#endif
#ifndef _longjmp
#define _longjmp longjmp
#endif
#ifndef fseeko
#define fseeko _fseeki64
#endif
#ifndef ftello
#define ftello _ftelli64
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

static inline int clock_gettime(int clk_id, struct timespec* tp) {
    if (!tp) return -1;
    if (clk_id == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq = {0};
        LARGE_INTEGER counter;
        if (freq.QuadPart == 0) {
            QueryPerformanceFrequency(&freq);
        }
        QueryPerformanceCounter(&counter);
        tp->tv_sec = (time_t)(counter.QuadPart / freq.QuadPart);
        tp->tv_nsec = (long)(((counter.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
        return 0;
    }
    return timespec_get(tp, TIME_UTC) == TIME_UTC ? 0 : -1;
}
