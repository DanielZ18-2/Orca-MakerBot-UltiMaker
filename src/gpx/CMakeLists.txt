#
# gpx - embedded copy of markwal/GPX 2.6.8 (https://github.com/markwal/GPX)
#
# Converts RepRap/Marlin G-code to the .x3g (s3g) stream the MakerBot legacy
# line speaks: Cupcake, Thing-O-Matic, Replicator 1/2/2X. Orca used to shell
# out to a `gpx` binary on PATH; embedding it removes that install step and
# lets a failed conversion report GPX's own diagnostics instead of an exit
# code.
#
# Layout follows src/glad: a plain static library with a PUBLIC include
# directory carrying exactly one header. gpx.h and its private config header
# stay PRIVATE - only GPXConvert.hpp is visible to the rest of the tree.
#
# Licensing: GPX is GPL-2.0-or-later (gpx.c, vector.c) with the
# shared/ helpers under 3-clause BSD. Both are compatible with Orca's AGPL-3.0.
# See LICENSE and README.orca.md in this directory.
#

set(GPX_SOURCES
    gpx.c
    vector.c
    shared/machine_config.c
    shared/opt.c
)

add_library(gpx STATIC
    ${GPX_SOURCES}
    GPXConvert.cpp
)

# GPX is C99: designated initialisers in the machine tables, // comments,
# declarations after statements.
set_target_properties(gpx PROPERTIES
    C_STANDARD          99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# PUBLIC exports this directory so libslic3r can write #include "GPXConvert.hpp".
# shared/ stays PRIVATE: gpx.h pulls machine.h from there, and nothing outside
# this target may reach those headers.
target_include_directories(gpx
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shared
)

# SERIAL_SUPPORT is intentionally left undefined: Orca never drives an S3G
# serial port itself, and defining it would drag in termios/USB code plus a
# platform matrix Orca does not need.
target_compile_definitions(gpx PRIVATE
    $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)

# Upstream is a 20k-line C code base that predates most of these warnings.
# Silence them here rather than patching the vendored sources - the goal is to
# keep the diff against upstream at zero apart from the config.h redirect.
if (MSVC)
    target_compile_options(gpx PRIVATE /wd4996 /wd4244 /wd4267)
else ()
    target_compile_options(gpx PRIVATE
        -Wno-unused-result
        -Wno-implicit-fallthrough
        -Wno-sign-compare
    )
    if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
        target_compile_options(gpx PRIVATE
            -Wno-unused-but-set-variable
            -Wno-format-truncation
            -Wno-stringop-truncation
        )
    endif ()
endif ()

find_library(GPX_MATH_LIBRARY m)
if (GPX_MATH_LIBRARY)
    target_link_libraries(gpx PRIVATE ${GPX_MATH_LIBRARY})
endif ()
