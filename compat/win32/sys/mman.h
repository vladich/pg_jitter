/*
 * compat/win32/sys/mman.h — mmap/mprotect/munmap shim for Windows
 *
 * Maps POSIX mmap() calls to Windows VirtualAlloc/VirtualProtect/VirtualFree.
 */
#ifndef PG_JITTER_COMPAT_SYS_MMAN_H
#define PG_JITTER_COMPAT_SYS_MMAN_H

#include <windows.h>
#include <errno.h>

/* Protection flags */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

/* Map flags */
#define MAP_PRIVATE     0x02
#define MAP_ANON        0x20
#define MAP_ANONYMOUS   MAP_ANON
#define MAP_JIT         0       /* no-op on Windows */
#define MAP_FIXED_NOREPLACE 0x100000    /* hint; best-effort on Windows */

#define MAP_FAILED ((void *)-1)

static inline DWORD
mman_prot_to_win32(int prot)
{
    if ((prot & PROT_EXEC) && (prot & PROT_WRITE))
        return PAGE_EXECUTE_READWRITE;
    if (prot & PROT_EXEC)
        return PAGE_EXECUTE_READ;
    if (prot & PROT_WRITE)
        return PAGE_READWRITE;
    if (prot & PROT_READ)
        return PAGE_READONLY;
    return PAGE_NOACCESS;
}

static inline void *
mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    DWORD protect = mman_prot_to_win32(prot);
    void *ptr;

    (void)fd;
    (void)offset;
    (void)flags;

    ptr = VirtualAlloc(addr, length, MEM_COMMIT | MEM_RESERVE, protect);
    if (ptr == NULL)
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    /* If caller wanted a specific address and got a different one, fail */
    if (addr != NULL && (flags & MAP_FIXED_NOREPLACE) && ptr != addr)
    {
        VirtualFree(ptr, 0, MEM_RELEASE);
        errno = EEXIST;
        return MAP_FAILED;
    }
    return ptr;
}

static inline int
mprotect(void *addr, size_t len, int prot)
{
    DWORD protect = mman_prot_to_win32(prot);
    DWORD old_protect;

    if (!VirtualProtect(addr, len, protect, &old_protect))
    {
        errno = EACCES;
        return -1;
    }
    return 0;
}

static inline int
munmap(void *addr, size_t length)
{
    (void)length;
    if (!VirtualFree(addr, 0, MEM_RELEASE))
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

#endif /* PG_JITTER_COMPAT_SYS_MMAN_H */
