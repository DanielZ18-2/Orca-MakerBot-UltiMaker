#pragma once
// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// MakerBotToolpath.hpp
//
// G-code → Birdwing JSON Toolpath converter.
// Analysed from MakerBot Print 4.10.1 mb_toolpath_parser.js
//
// JSON Toolpath format (print.jsontoolpath in .makerbot archive):
//   Array of {function, parameters, tags} objects.
//   Coordinate origin: BUILD PLATE CENTER (not corner like G-code).
//   feedrate: mm/s (not mm/min).
//   'a': extruder A delta in mm of filament per move.

#include <string>
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

struct BirdwingBuildVolume {
    double x { 300.0 };  // Z18
    double y { 305.0 };  // Z18
    double z { 457.0 };
    double layer_width { 0.4 };
};

// Per-conversion figures the caller needs for meta.json. In dual-extruder jobs
// extrusion[0]/[1] are the 'a' and 'b' axis totals - they must match
// meta.json's extrusion_distances_mm, which is how MakerBot Print writes them.
struct ToolpathStats {
    double extrusion[2] { 0.0, 0.0 };  // mm of filament per extruder axis
    int    tool_changes { 0 };
    bool   dual         { false };     // true once a real switch to T1 occurred
    // Bounding box of the EXTRUDING moves, i.e. the printed object - in
    // MakerBot coordinates (bed centre origin). meta.json's bounding_box used
    // to carry the whole build volume instead, which the firmware reads.
    double min_x { 0.0 }, max_x { 0.0 };
    double min_y { 0.0 }, max_y { 0.0 };
    double min_z { 0.0 }, max_z { 0.0 };
    bool   has_bbox { false };
};

// Convert Orca G-code file to Birdwing JSON toolpath string.
// Returns empty string on failure. Pass `stats` to receive the figures above.
std::string gcode_to_birdwing_jsontoolpath(
    const std::string&        gcode_path,
    const BirdwingBuildVolume& bv,
    double                    layer_height,
    std::string&              error,
    ToolpathStats*            stats = nullptr,
    // true -> Lava/Method format 3.0.0 (empty metadata on non-move commands),
    // false -> Birdwing 1.2.0. See MakerbotWriter.py in Cura: Method and
    // Replicator+ get print.jsontoolpath, the Sketch line gets print.gcode.
    bool                      lava_format = false);

} // namespace Slic3r
