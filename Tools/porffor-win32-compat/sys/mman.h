#pragma once
/* Minimal VirtualAlloc-backed mmap/mprotect shim for Porffor-generated C on Windows. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

#ifndef PROT_NONE
#define PROT_NONE 0x0
#endif
#ifndef PROT_READ
#define PROT_READ 0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x02
#endif
#ifndef MAP_FIXED
#define MAP_FIXED 0x10
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0x40
#endif
#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif

static inline void* mmap(void* addr, size_t len, int prot, int flags, int fd, long long offset) {
    (void)fd;
    (void)offset;
    if ((flags & MAP_FIXED) && prot == PROT_NONE) {
        if (!addr || len == 0) return addr;
        return VirtualFree(addr, len, MEM_DECOMMIT) ? addr : MAP_FAILED;
    }
    DWORD allocType = MEM_RESERVE;
    DWORD winProt = PAGE_NOACCESS;
    if (prot & (PROT_READ | PROT_WRITE)) {
        allocType |= MEM_COMMIT;
        winProt = PAGE_READWRITE;
    }
    void* p = VirtualAlloc(NULL, len, allocType, winProt);
    return p ? p : MAP_FAILED;
}

static inline int mprotect(void* addr, size_t len, int prot) {
    if (!addr || len == 0) return 0;
    if (prot == PROT_NONE) {
        return VirtualFree(addr, len, MEM_DECOMMIT) ? 0 : -1;
    }
    return VirtualAlloc(addr, len, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
}
