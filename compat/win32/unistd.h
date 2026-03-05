/*
 * compat/win32/unistd.h — minimal POSIX compatibility for Windows
 *
 * Provides sysconf(_SC_PAGESIZE) and other basics needed by pg_jitter.
 */
#ifndef PG_JITTER_COMPAT_UNISTD_H
#define PG_JITTER_COMPAT_UNISTD_H

#include <windows.h>
#include <io.h>
#include <process.h>

#ifndef F_OK
#define F_OK 0
#endif

#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 1
#endif

/* MSVC provides _access() instead of access() */
#ifndef access
#define access _access
#endif

static inline long
sysconf(int name)
{
    if (name == _SC_PAGESIZE)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (long)si.dwPageSize;
    }
    return -1;
}

#endif /* PG_JITTER_COMPAT_UNISTD_H */
