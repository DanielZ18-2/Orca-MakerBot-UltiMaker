/*
 * libgen.h - stand-in for MSVC, which has no such header
 *
 * gpx.c calls dirname() once, on a strdup'ed copy of the ini path, to find the
 * directory machine .ini files are searched in. This is POSIX dirname():
 * it may modify its argument and returns a pointer into it or to ".".
 * Both separators are accepted, since Windows paths reach GPX with either.
 */
#ifndef GPX_MSVC_LIBGEN_H
#define GPX_MSVC_LIBGEN_H

#include <string.h>

static __inline char *dirname(char *path)
{
    static char dot[] = ".";
    char *end;
    if (path == NULL || *path == '\0')
        return dot;
    end = path + strlen(path) - 1;
    while (end > path && (*end == '/' || *end == '\\'))      /* trailing separators */
        --end;
    while (end >= path && *end != '/' && *end != '\\')        /* last component */
        --end;
    if (end < path)
        return dot;                                           /* no separator at all */
    while (end > path && (end[-1] == '/' || end[-1] == '\\')) /* run of separators */
        --end;
    if (end == path)
        end[1] = '\0';                                        /* root: keep "/" */
    else if (end == path + 2 && path[1] == ':')
        end[1] = '\0';                                        /* "C:\" stays "C:\" */
    else
        *end = '\0';
    return path;
}

#endif /* GPX_MSVC_LIBGEN_H */
