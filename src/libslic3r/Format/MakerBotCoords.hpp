#ifndef slic3r_Format_MakerBotCoords_hpp_
#define slic3r_Format_MakerBotCoords_hpp_

#include <string>

namespace Slic3r {

class ConfigBase;

// ── Bettecke <-> Maschinenmitte ──────────────────────────────────────────────
//
// MakerBot firmware - Sailfish on the legacy line, the Birdwing/Lava stack on
// Method and Sketch - places the origin in the MIDDLE of the build platform.
// Cura states this explicitly per machine as `machine_center_is_zero: true`
// (resources/definitions/ultimaker_sketch*.def.json,
// makerbot_replicator*.def.json).
//
// Orca, like every slicer that describes a bed through `printable_area`, works
// in CORNER coordinates with the origin at the front left.
//
// The two systems must never appear in the same file. That was the defect this
// module removes: the machine profiles carried a start block written in
// machine coordinates, so the purge/anchor lines sat at negative Y while the
// object sat at positive Y. Orca's out-of-bounds check only looks at
// EXTRUDING paths - which is exactly what those lines are - so it reported the
// plate as overrun and the preview dragged its viewport off the bed, while the
// travel moves in the same block went unnoticed.
//
// The profiles are therefore kept entirely in corner coordinates, and the
// translation to machine coordinates happens once, here, on the way out:
//
//   * GPXExport.cpp        - before GPX turns the G-code into .x3g
//   * MakerBotExport.cpp   - before print.gcode goes into a Sketch .makerbot
//
// The Method/Lava line needs nothing: MakerBotToolpath.cpp has always
// subtracted the bed centre while building print.jsontoolpath.
namespace MakerBotCoords {

struct BedCentre
{
    bool   valid { false };
    double x     { 0.0 };
    double y     { 0.0 };
};

// Centre of the bounding box of `printable_area`. valid == false when the
// option is missing or degenerate - the caller must treat that as an error
// rather than shipping unshifted coordinates to a printer.
BedCentre bed_centre(const ConfigBase& config);

// Lines the printer must see but Orca must not.
//
// MakerBot's own start sequence declares the position after homing with
// `G92 X152 Y72 Z0 A0 B0`, and the homing block itself uses `G92 Z-5`
// (MakerBot Desktop 3.10, machine json "homing"/"start_position"). Both are
// correct for the printer and poison for Orca:
// GCodeProcessor::process_G92 turns a G92 into a coordinate-system origin
// (GCodeProcessor.cpp:4965), so every later move is drawn - and
// bounds-checked - 152/72 mm off the plate. That is what pushed the object
// outside the bed in the preview and raised "G-code path exceeds the boundary
// of the print bed" even though the exported file was fine.
//
// Such a line is therefore kept in the profile behind an "; MB_NATIVE "
// prefix. Orca reads a comment and keeps a clean origin; the exporter turns it
// back into the real command. Its values are already machine coordinates and
// are emitted verbatim, without the shift below.
extern const char* const NATIVE_PREFIX;   // "MB_NATIVE"

// Subtracts the bed centre from every X/Y of a motion command and expands the
// MB_NATIVE lines. See the .cpp for the command list and why it is a positive
// list. `native_lines`, if given, receives the number of MB_NATIVE lines found
// - zero on a legacy printer means the profiles were not migrated.
std::string to_machine_coordinates(const std::string& gcode,
                                   const BedCentre&   centre,
                                   size_t*            native_lines = nullptr);

bool file_to_machine_coordinates(const std::string& src_path,
                                 const std::string& dst_path,
                                 const BedCentre&   centre,
                                 std::string*       error,
                                 size_t*            native_lines = nullptr);

} // namespace MakerBotCoords
} // namespace Slic3r

#endif // slic3r_Format_MakerBotCoords_hpp_
