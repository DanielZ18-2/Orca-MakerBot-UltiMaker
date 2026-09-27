/*
 * sys/param.h - stand-in for MSVC, which has no such header
 *
 * shared/portable_endian.h includes it on Windows for the byte-order macros
 * only. Every Windows target Orca builds for (x64, arm64) is little-endian.
 */
#ifndef GPX_MSVC_SYS_PARAM_H
#define GPX_MSVC_SYS_PARAM_H

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN    4321
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER    LITTLE_ENDIAN
#endif

#endif /* GPX_MSVC_SYS_PARAM_H */
