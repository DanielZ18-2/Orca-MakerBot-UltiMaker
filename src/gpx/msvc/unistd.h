/*
 * unistd.h - stand-in for MSVC, which has no such header
 *
 * Only on the include path of the gpx target, and only for MSVC. Supplies
 * exactly what gpx.c takes from <unistd.h>: read/write/access, R_OK, ssize_t
 * and usleep. nanosleep is not needed: gpx_config.h leaves HAVE_NANOSLEEP
 * undefined on Windows, so GPX falls back to usleep.
 */
#ifndef GPX_MSVC_UNISTD_H
#define GPX_MSVC_UNISTD_H

#include <io.h>        /* read, write, access (POSIX names, deprecated but present) */
#include <process.h>
#include <stddef.h>
#include <stdint.h>

#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef F_OK
#define F_OK 0
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif

/* Same signature as the Win32 declaration in <synchapi.h>, so the two agree
   should <windows.h> ever be included into the same translation unit. */
__declspec(dllimport) void __stdcall Sleep(unsigned long dwMilliseconds);

static __inline int usleep(unsigned long usec)
{
    Sleep((usec + 999UL) / 1000UL);
    return 0;
}

#endif /* GPX_MSVC_UNISTD_H */
