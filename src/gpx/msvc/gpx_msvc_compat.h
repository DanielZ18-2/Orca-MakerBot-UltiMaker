/*
 * gpx_msvc_compat.h - POSIX names GPX uses that MSVC spells differently
 *
 * Force-included into every GPX C source when the compiler is MSVC (see
 * ../CMakeLists.txt). Upstream GPX builds on Windows only with MinGW, which
 * ships POSIX headers; MSVC does not. Keeping the adaptation here, and the
 * three stand-in headers next to it, leaves the vendored sources untouched.
 */
#ifndef GPX_MSVC_COMPAT_H
#define GPX_MSVC_COMPAT_H

#if defined(_MSC_VER)

#include <string.h>

#ifndef strcasecmp
#define strcasecmp  _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

#endif /* _MSC_VER */
#endif /* GPX_MSVC_COMPAT_H */
