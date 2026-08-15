#ifndef slic3r_GPXConvert_hpp_
#define slic3r_GPXConvert_hpp_

// Thin C++ facade over the embedded GPX library (markwal/GPX 2.6.8).
//
// Only this header is public. gpx.h and its private config header stay inside
// the gpx target so that nothing in libslic3r has to pull in GPX's C types,
// its BUFFER_MAX macros or a header called "config.h".
//
// Everything here is header-clean C++: no boost, no libslic3r types, so the
// gpx static library keeps zero dependencies on the rest of Orca.

#include <string>

namespace Slic3r {
namespace GPX {

// Figures GPX accumulates while converting; handy for the status bar and for
// sanity-checking an export without decompiling the .x3g.
struct ConvertStats
{
    double filament_mm { 0.0 };  // A + B axis total
    double duration_s  { 0.0 };
    double bytes       { 0.0 };  // size of the produced s3g/x3g stream
};

// Convert a G-code file to .x3g.
//
//   gcode_path  existing RepRap/Marlin flavour G-code (what Orca writes)
//   x3g_path    output file, created/truncated; parent directory must exist
//   machine     GPX machine code, e.g. "r2x", "r1d", "t7", "c3"
//   build_name  name shown on the printer's LCD; empty -> derived from x3g_path
//   ini_path    optional GPX .ini to load first; empty -> none
//   error       set to a human-readable message when the call returns false;
//               includes GPX's own log output, which is where its parser
//               complains about unsupported commands
//   stats       optional, filled on success
//
// Note on flavour: GPX defaults to reprapFlavor = 1, which is exactly what
// Orca's output needs. The CLI's "-g" switch (MakerBot/ReplicatorG flavour) is
// deliberately not reproduced here - it would route M106 to the extruder
// heatsink fan instead of the part cooling fan and make Tn non-sticky.
// See the comment block in GPXExport.cpp.
//
// Thread-safe: calls are serialised internally.
bool convert_gcode_to_x3g(const std::string& gcode_path,
                          const std::string& x3g_path,
                          const std::string& machine,
                          const std::string& build_name,
                          const std::string& ini_path,
                          std::string*       error,
                          ConvertStats*      stats = nullptr);

// True if `machine` is a machine code the embedded GPX knows.
bool is_known_machine(const std::string& machine);

// Newline-separated list of the machine codes GPX supports, for diagnostics
// and for the "unknown machine" error message.
std::string known_machines();

// Upstream version this copy was taken from, e.g. "2.6.8".
std::string version();

} // namespace GPX
} // namespace Slic3r

#endif // slic3r_GPXConvert_hpp_
