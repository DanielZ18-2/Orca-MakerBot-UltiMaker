/*
 * gpx_config.h - replacement for the autotools-generated config.h
 *
 * Upstream GPX (https://github.com/markwal/GPX, v2.6.8) is built with
 * autotools and includes a generated "config.h". Orca builds with CMake, so
 * this hand-written header supplies the handful of macros the GPX sources
 * actually reference. It is deliberately NOT called config.h: the GPX include
 * directory is private to the gpx target, but a header with that name is too
 * generic to risk shadowing anything else in the tree.
 *
 * The two upstream includes of "config.h" (gpx.h, shared/machine_config.c)
 * have been redirected to this file. That is the only edit made to the
 * upstream sources - see README.orca.md.
 *
 * SERIAL_SUPPORT is intentionally NOT defined: Orca drives printers over the
 * network or hands the .x3g to the user, it never talks to an S3G port
 * directly. Leaving it off removes the termios/USB code from the build.
 */
#ifndef __gpx_orca_config_h__
#define __gpx_orca_config_h__

/* Upstream release this copy was taken from. */
#define PACKAGE_NAME    "gpx"
#define PACKAGE_VERSION "2.6.8"
#define PACKAGE_STRING  "gpx 2.6.8"
#define VERSION         "2.6.8"

/* nanosleep() exists on every platform Orca targets except plain Win32. */
#if !defined(_WIN32) && !defined(_WIN64)
#define HAVE_NANOSLEEP 1
#endif

#endif /* __gpx_orca_config_h__ */
