// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// MakerBotExport.cpp – G-code → .makerbot / .ufp archive packer
//
// GOLD VERSION: Kombiniert Alpha (parse_header, build_birdwing_meta,
// BBox, extrusion_mass_g, extract_thumbnails, Lava-Support) mit
// Beta (pack_to_archive API, gcode_to_birdwing_jsontoolpath for
// correct Print 4.x format with relative.a=true).
//
// Core strategy: all speed and profile values are read directly
// from the G-code settings block (parse_header), no longer via
// PrintConfig casts - this avoids the DynamicPrintConfig/PrintConfig problem.

#include "MakerBotExport.hpp"
#include "MakerBotToolpath.hpp"
// Bed corner -> machine origin, see MakerBotCoords.hpp for the why.
#include "MakerBotCoords.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/LocalesUtils.hpp"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <regex>
#include <sstream>
#include <locale>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace MakerBotExport {

namespace {

// Same helper as GPXExport.cpp - std::getenv returns a raw pointer that may
// be null; wrapping it keeps the call sites readable.
std::string getenv_string(const char* name)
{
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

} // namespace


// ── Public: archive extension helper ────────────────────────────────────────

std::string get_archive_extension(GCodeFlavor flavor)
{
    switch (flavor) {
    case gcfMakerBotBirdwing:
    case gcfMakerBotLava:   return ".makerbot";
    case gcfMakerBotLegacy: return ".gcode";
    // gcfGriffin (.ufp) is handled entirely by UltimakerUFPExport - this
    // module is MakerBot-only. Never dispatched here in practice
    // (BackgroundSlicingProcess.cpp routes gcfGriffin straight to
    // UltimakerUFPExport::pack_to_archive).
    default:                return ".gcode";
    }
}

// ── Internal helpers ─────────────────────────────────────────────────────────


// Splits a comma separated per-extruder list from the G-code settings block.
static std::vector<std::string> split_list(const std::string& value)
{
    std::vector<std::string> out;
    boost::algorithm::split(out, value, boost::is_any_of(","));
    for (std::string& s : out)
        boost::algorithm::trim(s);
    return out;
}

// All settings read from the G-code settings block in a single pass
struct HeaderData
{
    std::string filament_type                = "PLA";
    std::string tool_type                    = "mk13";
    int         first_layer_temp             = 215;
    int         temperature                  = 215;
    int         chamber_temp                 = 0;
    // Orca writes the value and the switch separately; without the switch we
    // would heat a chamber the profile has turned off. See below.
    bool        chamber_active               = true;
    bool        saw_chamber_active           = false;
    double      layer_height                 = 0.20;
    double      first_layer_height           = 0.20;
    int         wall_loops                   = 2;
    double      infill_density               = 0.15;
    double      filament_density             = 1.24;
    double      filament_diameter            = 1.75;
    double      nozzle_diameter              = 0.4;
    double      retraction_length            = 0.5;   // Z18 Orca: 0.5mm
    double      retraction_speed             = 50.0;  // mm/s (process)
    double      filament_retraction_speed    = 40.0;  // mm/s (filament limit)
    double      deretraction_speed           = 30.0;  // mm/s (restart)
    double      z_hop                        = 0.0;
    double      z_offset                     = 0.0;
    double      travel_speed                 = 110.0;
    double      outer_wall_speed             = 49.0;
    double      inner_wall_speed             = 82.0;
    double      sparse_infill_speed          = 82.0;
    double      internal_solid_infill_speed  = 82.0;
    double      top_surface_speed            = 50.0;
    double      bridge_speed                 = 50.0;
    double      line_width                   = 0.4;
    int         fan_max_speed                = 100;
    int         close_fan_first_layers       = 1;
    int         enable_support               = 0;
    int         raft_layers                  = 0;
    int         duration_s                   = 0;
    int         num_layers                   = 0;
    double      total_filament_mm            = 0.0;
    // Previously hardcoded directly in build_birdwing_meta() - now parsed from
    // the actual G-code settings block like everything else in this struct.
    // Defaults below are the SAME values that used to be hardcoded, so any
    // gcode that for some reason lacks one of these comments still gets a
    // sane fallback instead of 0.
    double      max_volumetric_speed         = 5.0;   // filament_max_volumetric_speed
    double      default_acceleration         = 500.0; // default_acceleration
    double      travel_acceleration          = 2000.0;// travel_acceleration
    // Per-role acceleration and jerk, kept as the raw strings from the
    // settings block so a percentage ("50%") can be resolved against
    // default_acceleration afterwards. They feed meta.json's accel_overrides,
    // which is the ONLY channel through which an acceleration setting reaches
    // the Method series: the toolpath knows five command types and none of
    // them carries acceleration.
    std::map<std::string, std::string> accel_raw;
    // Gantry limits. Orca serialises these as "normal,stealth"; the first
    // entry is the normal mode. Defaults match Orca's own, not any machine.
    double      max_speed_x                  = 500.0; // machine_max_speed_x
    double      max_speed_y                  = 500.0; // machine_max_speed_y
    double      retract_restart_extra        = 0.1;   // retract_restart_extra (-> ooze_feedstock_distance)
    int         bed_temperature              = 0;     // bed_temperature / platform_temperature
    double      travel_speed_z               = 3.0;   // travel_speed_z
    // plate temperatures individually, selection later via curr_bed_type
    int         cool_plate_temp              = -1;
    int         eng_plate_temp               = -1;
    int         hot_plate_temp               = -1;
    int         textured_plate_temp          = -1;
    int         textured_cool_plate_temp     = -1;
    int         supertack_plate_temp         = -1;
    std::string curr_bed_type;
    bool        saw_filament_retraction_speed = false;
    // Filament overrides (Orca serialises them as nullable per-filament
    // vectors; an unset entry reads "nil"). Kept separate from the printer
    // values so an override that is absent leaves the printer value alone.
    double      filament_retraction_length      = 0.0;
    double      filament_deretraction_speed     = 0.0;
    double      filament_retract_restart_extra  = 0.0;
    bool        saw_filament_retraction_length     = false;
    bool        saw_filament_deretraction_speed    = false;
    bool        saw_filament_retract_restart_extra = false;
};

struct BBox
{
    double min_x =  std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double min_y =  std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();
    double min_z =  std::numeric_limits<double>::infinity();
    double max_z = -std::numeric_limits<double>::infinity();

    void update(double x, double y, double z)
    {
        min_x = std::min(min_x, x); max_x = std::max(max_x, x);
        min_y = std::min(min_y, y); max_y = std::max(max_y, y);
        min_z = std::min(min_z, z); max_z = std::max(max_z, z);
    }

    nlohmann::json to_json() const
    {
        auto fz = [](double v) { return std::isfinite(v) ? v : 0.0; };
        return {{"x_min", fz(min_x)}, {"x_max", fz(max_x)},
                {"y_min", fz(min_y)}, {"y_max", fz(max_y)},
                {"z_min", fz(min_z)}, {"z_max", fz(max_z)}};
    }
};

struct ThumbnailBlob { int width = 0, height = 0; std::string bytes; };

// The settings block serialises filament_type for ALL extruders in one
// string: a dual-head machine yields "PLA;PLA". meta["material"] is a single
// name - the reference file writes "abs-wss1", not "abs-wss1;abs-wss1" - so
// the first entry is what belongs there. meta["materials"] is built
// separately from ConfigOptionStrings and already carries one entry per
// extruder.
//
// Measured on Mr_Jaws_PLA_1h2m.makerbot (Method X): "material" read
// "pla;pla" while "materials" correctly read ["pla", "pla"]. Every test file
// before it came from a single-extruder machine, where the string has no
// separator and the bug is invisible.
static std::string first_filament_type(const std::string& filament_type)
{
    const size_t cut = filament_type.find_first_of(";,");
    std::string t = cut == std::string::npos ? filament_type
                                             : filament_type.substr(0, cut);
    boost::algorithm::trim(t);
    return t;
}


static double parse_double_safe(const std::string& s, double fb)
{
    // Locale-independent via upstream helper (fast_float, ALWAYS reads '.' as
    // decimal separator, independent of the process locale -- earlier cause of wrong
    // filament mass / layer_height=0 under de_DE). The helper leaves the value
    // uninitialized on failure and returns pos==0; so check pos and
    // return the fallback.
    size_t pos = 0;
    const double v = string_to_double_decimal_point(s, &pos);
    return (pos != 0 && std::isfinite(v)) ? v : fb;
}

static int parse_int_safe(const std::string& s, int fb)
{
    try { size_t p; return std::stoi(s, &p); }
    catch (...) { return fb; }
}

static int hms_to_seconds(const std::string& s)
{
    // Parse "3h 52m 45s" or "3h52m45s" or "13833" (pure seconds)
    const std::regex part_re(R"((\d+)\s*([dhms]))", std::regex::icase);
    int total = 0;
    bool found_unit = false;
    for (std::sregex_iterator it(s.begin(), s.end(), part_re), end; it != end; ++it) {
        found_unit = true;
        const int value = parse_int_safe((*it)[1].str(), 0);
        const char unit = static_cast<char>(std::tolower((*it)[2].str()[0]));
        if      (unit == 'd') total += value * 86400;
        else if (unit == 'h') total += value * 3600;
        else if (unit == 'm') total += value * 60;
        else if (unit == 's') total += value;
    }
    if (!found_unit) total = parse_int_safe(s, 0);
    return total;
}

// Reads one filament override value.
//
// Orca writes these as nullable per-filament vectors, so the serialised
// form is "nil", "4", "nil,4" or "4,nil". The .makerbot header describes a
// single tool with a single material, so the first entry that is not nil is
// the one that applies. Returns false when the whole vector is nil - the
// printer value then stands untouched, which is also what the slicer does.
static bool parse_override(const std::string& s, double& out)
{
    size_t start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(',', start);
        std::string part = (comma == std::string::npos)
            ? s.substr(start)
            : s.substr(start, comma - start);
        boost::algorithm::trim(part);
        if (!part.empty() && part != "nil") {
            size_t pos = 0;
            const double v = string_to_double_decimal_point(part, &pos);
            if (pos != 0 && std::isfinite(v)) { out = v; return true; }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

// Single-pass G-code parser:
//   - Collects all settings from "; key = value" comments
//   - Accumulates positive E values for filament total (relative E mode)
//   - Detects tool_type from "; makerbot_tool_type = mk13" comments
static HeaderData parse_header(const std::string& gcode_path, const PrintConfig& config)
{
    HeaderData h;

    // Smart extruder type from PrintConfig if available
    {
        const auto* opt = config.option("smart_extruder_type");
        if (opt) {
            try {
                const auto* ss = dynamic_cast<const ConfigOptionStrings*>(opt);
                if (ss && !ss->values.empty() && !ss->values[0].empty()
                    && ss->values[0] != "none")
                    h.tool_type = ss->values[0];
            } catch (...) {}
        }
    }

    std::ifstream gf(gcode_path);
    if (!gf.is_open()) return h;

    std::string line;
    while (std::getline(gf, line)) {
        // ── Filament accumulation (G0/G1 with positive E) ───────────────
        if (line.size() >= 2 && line[0] == 'G' && (line[1] == '0' || line[1] == '1')) {
            const size_t e_pos = line.find('E');
            if (e_pos != std::string::npos) {
                const size_t semi = line.find(';');
                if (semi == std::string::npos || semi > e_pos) {
                    try {
                        const double e = parse_double_safe(line.substr(e_pos + 1), 0.0);
                        // signed accumulation: retraction(-) and restart(+) cancel
                        // auf -> netto = echter Verbrauch (deckt sich mit Orca-GUI).
                        h.total_filament_mm += e;
                    } catch (...) {}
                }
            }
            continue;
        }

        // ── Settings comments ────────────────────────────────────────────
        if (line.empty() || line[0] != ';') continue;

        // Skip thumbnail blocks
        if (line.find("thumbnail begin") != std::string::npos) {
            while (std::getline(gf, line) && line.find("thumbnail end") == std::string::npos) {}
            continue;
        }

        std::string comment = line.substr(1);
        boost::algorithm::trim(comment);

        // makerbot_tool_type = mk13
        {
            std::string cl = boost::algorithm::to_lower_copy(comment);
            if (cl.rfind("makerbot_tool_type", 0) == 0) {
                const size_t eq = comment.find('=');
                if (eq != std::string::npos) {
                    std::string tool = comment.substr(eq + 1);
                    boost::algorithm::trim(tool);
                    if (!tool.empty()) h.tool_type = tool;
                }
                continue;
            }
        }

        // key = value
        size_t sep = comment.find('=');
        if (sep == std::string::npos) sep = comment.find(':');
        if (sep == std::string::npos) continue;

        std::string key = comment.substr(0, sep);
        std::string val = comment.substr(sep + 1);

        // Drop unit suffix like "(mm)"
        const size_t paren = key.find('(');
        if (paren != std::string::npos) key = key.substr(0, paren);

        boost::algorithm::trim(key);
        boost::algorithm::trim(val);
        boost::algorithm::to_lower(key);

        // Map keys to HeaderData fields
        if      (key == "nozzle_temperature_initial_layer" || key == "first_layer_temperature")
                                                h.first_layer_temp             = parse_int_safe(val, h.first_layer_temp);
        else if (key == "nozzle_temperature" || key == "temperature")
                                                h.temperature                  = parse_int_safe(val, h.temperature);
        else if (key == "chamber_temperature") h.chamber_temp                  = parse_int_safe(val, h.chamber_temp);
        else if (key == "activate_chamber_temp_control") {
            // Per-extruder list, e.g. "0,0" or "1,0". Orca treats the chamber as
            // active when ANY extruder asks for it (GCode.cpp:3019 ORs them), and
            // only then emits the heating commands (GCode.cpp:3138 / 3518).
            h.saw_chamber_active = true;
            h.chamber_active     = false;
            for (const std::string& part : split_list(val))
                if (parse_int_safe(part, 0) != 0) { h.chamber_active = true; break; }
        }
        else if (key == "total layer number")  h.num_layers                    = parse_int_safe(val, h.num_layers);
        else if (key == "layer_height")        h.layer_height                  = parse_double_safe(val, h.layer_height);
        else if (key == "first_layer_height")  h.first_layer_height            = parse_double_safe(val, h.first_layer_height);
        else if (key == "line_width")          h.line_width                    = parse_double_safe(val, h.line_width);
        else if (key == "wall_loops")          h.wall_loops                    = parse_int_safe(val, h.wall_loops);
        else if (key == "sparse_infill_density") {
            boost::algorithm::erase_all(val, "%");
            h.infill_density = parse_double_safe(val, h.infill_density * 100.0) / 100.0;
        }
        else if (key == "filament_density")    h.filament_density              = parse_double_safe(val, h.filament_density);
        else if (key == "filament_diameter")   h.filament_diameter             = parse_double_safe(val, h.filament_diameter);
        else if (key == "nozzle_diameter")     h.nozzle_diameter               = parse_double_safe(val, h.nozzle_diameter);
        else if (key == "filament_type")       h.filament_type                 = boost::algorithm::to_upper_copy(val);
        else if (key == "z_offset")            h.z_offset                      = parse_double_safe(val, h.z_offset);
        else if (key == "retraction_length")   h.retraction_length             = parse_double_safe(val, h.retraction_length);
        else if (key == "retraction_speed")    h.retraction_speed              = parse_double_safe(val, h.retraction_speed);
        else if (key == "filament_retraction_speed") {
            h.saw_filament_retraction_speed =
                parse_override(val, h.filament_retraction_speed);
        }
        else if (key == "filament_retraction_length") {
            h.saw_filament_retraction_length =
                parse_override(val, h.filament_retraction_length);
        }
        else if (key == "filament_deretraction_speed") {
            h.saw_filament_deretraction_speed =
                parse_override(val, h.filament_deretraction_speed);
        }
        else if (key == "filament_retract_restart_extra") {
            h.saw_filament_retract_restart_extra =
                parse_override(val, h.filament_retract_restart_extra);
        }
        else if (key == "deretraction_speed")  h.deretraction_speed            = parse_double_safe(val, h.deretraction_speed);
        else if (key == "z_hop")               h.z_hop                         = parse_double_safe(val, h.z_hop);
        else if (key == "travel_speed")        h.travel_speed                  = parse_double_safe(val, h.travel_speed);
        else if (key == "outer_wall_speed")    h.outer_wall_speed              = parse_double_safe(val, h.outer_wall_speed);
        else if (key == "inner_wall_speed")    h.inner_wall_speed              = parse_double_safe(val, h.inner_wall_speed);
        else if (key == "sparse_infill_speed") h.sparse_infill_speed           = parse_double_safe(val, h.sparse_infill_speed);
        else if (key == "internal_solid_infill_speed") h.internal_solid_infill_speed = parse_double_safe(val, h.internal_solid_infill_speed);
        else if (key == "top_surface_speed")   h.top_surface_speed             = parse_double_safe(val, h.top_surface_speed);
        else if (key == "bridge_speed")        h.bridge_speed                  = parse_double_safe(val, h.bridge_speed);
        else if (key == "fan_max_speed")       h.fan_max_speed                 = parse_int_safe(val, h.fan_max_speed);
        else if (key == "close_fan_the_first_x_layers") h.close_fan_first_layers = parse_int_safe(val, h.close_fan_first_layers);
        else if (key == "enable_support")      h.enable_support                = parse_int_safe(val, h.enable_support);
        else if (key == "raft_layers")         h.raft_layers                   = parse_int_safe(val, h.raft_layers);
        else if (key == "filament_max_volumetric_speed") h.max_volumetric_speed = parse_double_safe(val, h.max_volumetric_speed);
        else if (key == "default_acceleration") h.default_acceleration         = parse_double_safe(val, h.default_acceleration);
        else if (key == "travel_acceleration")  h.travel_acceleration          = parse_double_safe(val, h.travel_acceleration);
        else if (key == "outer_wall_acceleration"          ||
                 key == "inner_wall_acceleration"          ||
                 key == "sparse_infill_acceleration"       ||
                 key == "internal_solid_infill_acceleration" ||
                 key == "top_surface_acceleration"         ||
                 key == "support_acceleration"             ||
                 key == "initial_layer_acceleration"       ||
                 key == "default_jerk"                     ||
                 key == "outer_wall_jerk"                  ||
                 key == "inner_wall_jerk"                  ||
                 key == "infill_jerk"                      ||
                 key == "top_surface_jerk"                 ||
                 key == "travel_jerk")   h.accel_raw[key]                        = val;
        else if (key == "machine_max_speed_x")  h.max_speed_x                  = parse_double_safe(val, h.max_speed_x);
        else if (key == "machine_max_speed_y")  h.max_speed_y                  = parse_double_safe(val, h.max_speed_y);
        else if (key == "retract_restart_extra") h.retract_restart_extra       = parse_double_safe(val, h.retract_restart_extra);
        else if (key == "bed_temperature" || key == "bed_temperature_initial_layer")
                                                h.bed_temperature               = parse_int_safe(val, h.bed_temperature);
        else if (key == "cool_plate_temp")     h.cool_plate_temp                = parse_int_safe(val, 0);
        else if (key == "eng_plate_temp")      h.eng_plate_temp                 = parse_int_safe(val, 0);
        else if (key == "hot_plate_temp")      h.hot_plate_temp                 = parse_int_safe(val, 0);
        else if (key == "textured_plate_temp") h.textured_plate_temp            = parse_int_safe(val, 0);
        else if (key == "textured_cool_plate_temp") h.textured_cool_plate_temp  = parse_int_safe(val, 0);
        else if (key == "supertack_plate_temp") h.supertack_plate_temp          = parse_int_safe(val, 0);
        else if (key == "curr_bed_type")       h.curr_bed_type                  = val;
        else if (key == "travel_speed_z")      h.travel_speed_z                = parse_double_safe(val, h.travel_speed_z);
        else if (key.find("estimated printing time") != std::string::npos)
                                               h.duration_s                    = std::max(h.duration_s, hms_to_seconds(val));
    }

    // The chamber is only heated when the profile asks for it.
    //
    // parse_header() used to read chamber_temperature alone and pass it
    // straight into meta.json, so a profile with the switch turned off still
    // produced a heated chamber. Measured on a Z18 PLA tower: the filament
    // profile carried activate_chamber_temp_control = 0 and the file still
    // said chamber_temperature = 40.
    //
    // If the key is absent - older files, a different slicer - keep the old
    // behaviour rather than silently disabling a chamber someone may need.
    if (h.saw_chamber_active && !h.chamber_active)
        h.chamber_temp = 0;

    // select the plate temperature matching the chosen build plate.
    // Without this, hot_plate_temp always ends up in meta.json, even if the
    // user selected the unheated "Cool Plate".
    {
        std::string bt = boost::algorithm::to_lower_copy(h.curr_bed_type);
        int sel = -1;
        if      (bt.find("supertack")  != std::string::npos) sel = h.supertack_plate_temp;
        else if (bt.find("textured")   != std::string::npos &&
                 bt.find("cool")       != std::string::npos) sel = h.textured_cool_plate_temp;
        else if (bt.find("textured")   != std::string::npos) sel = h.textured_plate_temp;
        else if (bt.find("engineering")!= std::string::npos) sel = h.eng_plate_temp;
        else if (bt.find("high temp")  != std::string::npos) sel = h.hot_plate_temp;
        else if (bt.find("cool")       != std::string::npos) sel = h.cool_plate_temp;
        if (sel < 0) sel = h.hot_plate_temp;          // fallback, unchanged
        if (sel >= 0) h.bed_temperature = sel;
    }

    // Effective retract rate = min(process, filament_limit)
    // The cap may only apply when the filament profile actually
    // set its own limit. Otherwise the default value (40) would
    // throttle the correct machine speed.
    // A filament override REPLACES the printer value - it is not a cap.
    // See PrintConfig.cpp, compute_filament_override_value():
    //     opt_copy->apply_override(opt_new_filament, f_maps);
    // The earlier std::min() here reported the printer's slower speed
    // whenever a filament profile asked for a faster one.
    //
    // Reading these at all matters because the CONFIG_BLOCK in the G-code
    // is written from Print::full_print_config(), which still holds the raw
    // printer values; Print applies the overrides to m_config only. So
    // "retraction_length" alone describes a retraction the printer never
    // performs as soon as the filament profile overrides it.
    if (h.saw_filament_retraction_length)
        h.retraction_length = h.filament_retraction_length;
    if (h.saw_filament_retract_restart_extra)
        h.retract_restart_extra = h.filament_retract_restart_extra;
    if (h.saw_filament_deretraction_speed)
        h.deretraction_speed = h.filament_deretraction_speed;
    if (!h.saw_filament_retraction_speed)
        h.filament_retraction_speed = h.retraction_speed;

    return h;
}

static double extrusion_mass_g(double extrusion_mm, double filament_diameter_mm, double density_g_cm3)
{
    const double r = (filament_diameter_mm / 2.0) / 10.0; // cm
    const double l = extrusion_mm / 10.0;                  // cm
    return l * 3.14159265358979323846 * r * r * density_g_cm3;
}

// Defined below, but the Birdwing branch needs it: its gaggle used to carry
// a build volume hardcoded to the Z18, which every other Birdwing machine
// then reported as its own.
static nlohmann::json machine_bounds(const PrintConfig& config);

static nlohmann::json build_birdwing_meta(
    const PrintConfig&  config,
    const std::string&  bot_type,
    const HeaderData&   h,
    const BBox&         bbox,
    double              total_extrusion,
    int                 command_count,
    const std::string&  project_name)
{
    // Same separator problem as in the Lava branch: a dual-head
    // Legacy machine (Replicator 2X, Original Dual) reports
    // "PLA;PLA" here.
    const std::string mat_first = first_filament_type(h.filament_type);
    const std::string mat_up  = mat_first.empty() ? "PLA" : boost::algorithm::to_upper_copy(mat_first);
    const std::string mat_lo  = boost::algorithm::to_lower_copy(mat_up);
    const std::string tool    = h.tool_type.empty() || h.tool_type == "none" ? "mk13" : h.tool_type;
    const double      mass_g  = extrusion_mass_g(total_extrusion, h.filament_diameter, h.filament_density);
    // Effective retract rate: min(process speed, filament limit)
    const double ret_rate  = std::max(1.0, h.filament_retraction_speed);
    const double rest_rate = std::max(1.0, h.deretraction_speed);

    nlohmann::json meta;
    meta["bot_type"]                 = bot_type.empty() ? "z18_6" : bot_type;
    meta["bounding_box"]             = bbox.to_json();
    meta["chamber_temperature"]      = h.chamber_temp;
    meta["commanded_duration_s"]     = h.duration_s;
    meta["duration_s"]               = h.duration_s;
    // Was {first_layer_temp, temperature} - the first layer and the other
    // layers of the SAME extruder, which reads like a plausible pair on a
    // single-head machine and is still the wrong field. The Lava branch had
    // the identical fault (O118); this is its Birdwing half.
    //
    // MakerBot Print's own file for the Z18 (MB_Print_Z18_Temmp_Tower)
    // writes extruder_temperature 215 and extruder_temperatures [215]:
    // one entry per extruder, and the PRINT temperature, not the
    // first-layer one.
    //
    // Every Birdwing machine carries exactly one extruder (the mk13 family:
    // Z18, Replicator+, Mini, Mini+, 5th Gen), so the list has exactly one
    // entry. Same source as the Lava branch: nozzle_temperature per
    // extruder, falling back to the parsed print temperature.
    int birdwing_temp = h.temperature;
    if (const auto* nt_opt = config.option("nozzle_temperature")) {
        const auto* nt = dynamic_cast<const ConfigOptionInts*>(nt_opt);
        if (nt && !nt->values.empty())
            birdwing_temp = nt->values.front();
    }
    meta["extruder_temperature"]     = birdwing_temp;
    meta["extruder_temperatures"]    = nlohmann::json::array({birdwing_temp});
    meta["extrusion_distance_mm"]    = total_extrusion;
    meta["extrusion_distances_mm"]   = nlohmann::json::array({total_extrusion});
    meta["extrusion_mass_g"]         = mass_g;
    meta["extrusion_masses_g"]       = nlohmann::json::array({mass_g});
    meta["extrusion_mass_a_grams"]   = mass_g;
    meta["extrusion_distance_a_mm"]  = total_extrusion;
    meta["material"]                 = mat_up;
    meta["materials"]                = nlohmann::json::array({mat_up});
    meta["model_counts"]             = nlohmann::json::array({nlohmann::json{{"count",1},{"name","instance0"}}});
    meta["name"]                     = project_name;
    meta["num_tool_changes"]         = 0; // TODO: not yet tracked - needs actual T0/T1 tool-change counting from the gcode (single-extruder prints are correctly 0; multi-material prints will under-report)
    meta["num_z_layers"]             = h.num_layers;
    meta["num_z_transitions"]        = h.num_layers > 0 ? h.num_layers + 1 : 0;
    meta["platform_temperature"]     = h.bed_temperature; // was: hardcoded 0
    meta["tool_type"]                = tool;
    meta["tool_types"]               = nlohmann::json::array({tool});
    meta["total_commands"]           = command_count;
    meta["version"]                  = "1.2.0";
    meta["uses_raft"]                = h.raft_layers > 0;

    // printer_settings – human-readable summary
    {
        nlohmann::json ps;
        ps["layer_height"]         = h.layer_height;
        ps["infill"]               = h.infill_density;
        ps["shells"]               = h.wall_loops;
        ps["support"]              = h.enable_support > 0;
        ps["raft"]                 = h.raft_layers > 0;
        ps["materials"]            = nlohmann::json::array({mat_up});
        // Same field, same rule as in meta above: one entry per extruder.
        ps["extruder_temperatures"]= nlohmann::json::array({birdwing_temp});
        ps["first_layer_height"]   = h.first_layer_height;
        ps["chamber_temperature"]  = h.chamber_temp;
        // Birdwing stores this in printer_settings; it does not validate it.
        // Checked against the 2.6.3.736 firmware images: the root filesystem
        // carries no list of slicer names, and the only slicer-related value
        // kaiten reads is extra_slicer_settings.plate_variability in
        // processes/printprocess.py. libtinything.so carries the bare key
        // "slicer", i.e. it stores the field rather than checking it. Files
        // written by MakerBot's own software frequently omit the field
        // altogether. Replaces a diagnostic value set on 2026-06-28 that was
        // never rolled back.
        ps["slicer"]               = "ORCA_SLICER";
        meta["printer_settings"]   = ps;
    }

    // extruder_profiles (for firmware: per-material speeds and hw params)
    {
        // hw limits based on actual profile speeds (no artificial caps)
        nlohmann::json ext_hw;
        // BUG FIX (2026-06-19): these four were hardcoded literals, completely
        // ignoring the user's actual filament/process settings - exactly the
        // "static defaults instead of live Orca values" problem reported.
        // All four now come from HeaderData, parsed from the G-code's own
        // settings block (the same mechanism every other field in this
        // function already uses) instead of being baked in here.
        ext_hw["feed_diameter"]     = h.filament_diameter;       // was: hardcoded 1.77
        ext_hw["nozzle_diameter"]   = h.nozzle_diameter;
        ext_hw["max_flow_rate"]     = h.max_volumetric_speed;    // was: hardcoded 5.0
        ext_hw["ooze_feedstock_distance"] = h.retract_restart_extra; // was: hardcoded 0.1
        ext_hw["retract_distance"]  = h.retraction_length;
        ext_hw["retract_rate"]      = ret_rate;
        ext_hw["restart_rate"]      = rest_rate;
        ext_hw["temperature"]       = h.temperature;
        // No slip_compensation_table – Orca calibration handles this
        // No acceleration block. The three keys this fork used to write -
        // normal_move, during_retract, after_retract - are not documented in
        // any MakerBot source we hold, and no .makerbot produced by MakerBot's
        // own software carries them. Their values were wrong regardless:
        // default_acceleration and travel_acceleration describe the gantry in
        // XY, while during_retract and after_retract would describe the
        // extruder axis. Without the block the firmware uses its own
        // constants, which is what it does today anyway.

        nlohmann::json ext_materials;
        ext_materials[mat_lo] = ext_hw;

        nlohmann::json ep;
        ep["materials"] = ext_materials;

        nlohmann::json machine_config;
        machine_config["extruder_profiles"] = nlohmann::json{{tool, ep}};
        machine_config["gantry_configuration"] = nlohmann::json{
            {"max_outer_shell_speed", h.outer_wall_speed},
            {"max_inner_shell_speed", h.inner_wall_speed},
            {"max_fill_speed",        h.sparse_infill_speed},
            {"travel_speed_xy",       h.travel_speed},
            {"travel_speed_z",        h.travel_speed_z}, // was: hardcoded 3.0
            // was: hardcoded 175/175, a figure from no profile we ship
            {"max_speed_mm_per_second", nlohmann::json{{"x",h.max_speed_x},
                                                       {"y",h.max_speed_y},
                                                       {"z",h.travel_speed_z}}}
        };
        meta["machine_config"] = machine_config;
    }

    // miracle_config – Birdwing slicer profile
    {
        nlohmann::json extrusion_profiles;
        extrusion_profiles["outlines"]      = {{"feedrate", h.outer_wall_speed}};
        extrusion_profiles["insets"]        = {{"feedrate", h.inner_wall_speed}};
        extrusion_profiles["solid"]         = {{"feedrate", h.internal_solid_infill_speed}};
        extrusion_profiles["sparse"]        = {{"feedrate", h.sparse_infill_speed}};
        extrusion_profiles["floor_surface"] = {{"feedrate", h.top_surface_speed}};
        extrusion_profiles["roof_surface"]  = {{"feedrate", h.top_surface_speed}};
        extrusion_profiles["bridges"]       = {{"feedrate", h.bridge_speed}};

        nlohmann::json ext_profile;
        ext_profile["feedDiameter"]      = h.filament_diameter; // was: hardcoded 1.77
        ext_profile["nozzleDiameter"]    = h.nozzle_diameter;
        ext_profile["temperature"]       = h.temperature;
        ext_profile["retractDistance"]   = h.retraction_length;
        ext_profile["retractRate"]       = ret_rate;
        ext_profile["restartRate"]       = rest_rate;
        ext_profile["zHopDistance"]      = h.z_hop;
        ext_profile["extrusionProfiles"] = extrusion_profiles;

        nlohmann::json gaggle;
        gaggle["_baseLayer"]             = h.raft_layers > 0 ? "raft" : "model";
        gaggle["_printMode"]             = "balanced";
        gaggle["_supportType"]           = "modelBreakaway";
        gaggle["baseLayerHeight"]        = h.first_layer_height;
        gaggle["bedZOffset"]             = 0.0; // NOTE: distinct from h.z_offset (nozzle/filament Z calibration) - bed-leveling Z offset isn't currently exposed in Orca's config model, left as firmware default rather than guessing a wrong mapping
        gaggle["chamberTemp"]            = h.chamber_temp;
        gaggle["doFanCommand"]           = true;
        gaggle["doExponentialDeceleration"] = true;
        gaggle["doRaft"]                 = h.raft_layers > 0;
        gaggle["doSupport"]              = h.enable_support > 0;
        gaggle["fanLayer"]               = h.close_fan_first_layers;
        gaggle["fanSpeed"]               = std::max(0.0, std::min(1.0, h.fan_max_speed / 100.0));
        gaggle["layerHeight"]            = h.layer_height;
        // Was hardcoded to {150.0, 152.5, -150.0, -152.5} - the Z18's
        // 300 x 305 plate - which every Birdwing machine then reported as
        // its own. Measured on five of our own files: the Replicator+
        // (295 x 195) carried the Z18's numbers. MakerBot Print writes no
        // machineBounds inside gaggles at all; the field is ours, so it is
        // corrected here rather than removed, which would be an untested
        // change to a container the printer accepts today.
        {
            const nlohmann::json mb = machine_bounds(config);
            if (!mb.is_null())
                gaggle["machineBounds"] = mb;
        }
        gaggle["platformTemp"]           = h.bed_temperature; // was: hardcoded 0
        gaggle["travelSpeedXY"]          = h.travel_speed;
        gaggle["travelSpeedZ"]           = h.travel_speed_z; // was: hardcoded 3
        gaggle["numberOfShells"]         = h.wall_loops;
        gaggle["infillDensity"]          = h.infill_density;
        gaggle["extruderProfiles"]       = nlohmann::json::array({ext_profile});

        nlohmann::json mc;
        mc["_bot"]       = meta["bot_type"];
        mc["_extruders"] = nlohmann::json::array({tool});
        mc["_materials"] = nlohmann::json::array({mat_lo});
        // MakerBot Print writes the real path of the MiracleGrue profile it
        // used. We have no such file, and a path that exists nowhere is
        // worse than none - same class as printer_settings.slicer =
        // "SIMPLIFY3D" (B90). The printer cannot resolve either, so name
        // what actually produced the settings.
        mc["configPath"] = "OrcaSlicer";
        mc["doRaft"]     = h.raft_layers > 0;
        mc["doSupport"]  = h.enable_support > 0;
        mc["layerHeight"]= h.layer_height;
        mc["numberOfShells"]  = h.wall_loops;
        mc["infillDensity"]   = h.infill_density;
        mc["gaggles"]["default"] = gaggle;
        meta["miracle_config"] = mc;
    }

    return meta;
}

// MakerBot's own material identifiers for the .makerbot header.
//
// Our filament_type is Orca's vocabulary ("PETG", "PA-CF", "ABS-R").  The
// header carries MakerBot's, and the two are NOT the same string in lower
// case.  The authoritative table is Cura's cura/PrinterOutput/FormatMaps.py,
// MATERIAL_MAP - the map UltiMaker's own MakerbotWriter applies before it
// writes meta.json (MakerbotWriter.py lines 155, 166-168):
//
//     abs        ABS           pla         PLA
//     abs-cf10   ABS-CF        pva         PVA
//     abs-wss1   ABS-R         wss1        RapidRinse
//     asa        ASA           sr30        SR-30
//     nylon      Nylon         pet         PETG
//     nylon-cf   Nylon CF      nylon12-cf  Nylon 12 CF
//
// Seven of the eleven names this fork can produce were wrong: lower-casing
// "PETG" yields "petg" where the printer expects "pet", and "ABS-R" yields
// "abs-r" where it expects "abs-wss1".
//
// "im-pla" (Tough PLA) is deliberately absent.  Tough PLA carries
// filament_type "PLA" here because it is a product name, not a material
// type; filament_type alone cannot tell the two apart.
//
// Types the table does not know - TPU, PC-ABS, BVOH, CoPE - fall through
// lower-cased.  That is not documented as correct, but it keeps them in the
// file instead of dropping them.
//
// This applies to the Lava branch only.  Cura does not cover Birdwing
// machines at all; there MakerBot Desktop and MakerBot Print are the
// reference and they write the name in upper case.
static std::string lava_material_id(const std::string& filament_type)
{
    static const std::map<std::string, std::string> kMakerBotMaterialIds = {
        {"ABS",        "abs"},
        {"ABS-CF",     "abs-cf10"},
        {"ABS-R",      "abs-wss1"},
        {"ASA",        "asa"},
        {"PA",         "nylon"},
        {"PA-CF",      "nylon-cf"},
        {"PA12-CF",    "nylon12-cf"},
        {"PET",        "pet"},
        {"PETG",       "pet"},
        {"PLA",        "pla"},
        {"PVA",        "pva"},
        {"RapidRinse", "wss1"},
        {"SR-30",      "sr30"},
    };
    const std::string first = first_filament_type(filament_type);
    const std::string t = first.empty() ? std::string("PLA") : first;
    const auto it = kMakerBotMaterialIds.find(t);
    if (it != kMakerBotMaterialIds.end())
        return it->second;
    return boost::algorithm::to_lower_copy(t);
}

// ── accel_overrides: the only way acceleration reaches a Method ──────────
//
// Wording taken from a Cura-produced reference file
// (UMMXL_2023_Testwuerfel_20x20.makerbot, Method XL, CuraEngine 5.12.0):
//
//   "accel_overrides": { "bead_mode": {
//       "Travel Move": {"rate_mm_per_s_sq": {"x":5000,"y":5000},
//                       "max_speed_change_mm_per_s": {"x":12.5,"y":12.5}},
//       "FILL_0", "PRIME_TOWER_0", "TOP_SURFACE_0", "SUPPORT_0",
//       "SUPPORT_INTERFACE_0", "WALL_OUTER_0", "WALL_INNER_0", "SKIRT_0",
//       ... the same eight with _1 ...
//   }}
//
// The role names are Cura's vocabulary; "Travel Move" is a MakerBot tag.
// Mixed, and the firmware matches these exact strings - an invented name is
// silently ignored. There is no name for bridges, bottom surfaces, the first
// layer or gap fill; those keep the machine's own value.
//
// Cura writes the same number into all sixteen role entries because it has a
// single acceleration_print setting. Our profiles differentiate five roles,
// so this block is where the fork can actually do better than Cura.

static double accel_from(const HeaderData& h, const char* key, double fallback)
{
    const auto it = h.accel_raw.find(key);
    if (it == h.accel_raw.end() || it->second.empty())
        return fallback;
    const std::string& s = it->second;
    const double v = parse_double_safe(s, -1.0);
    if (v < 0.0)
        return fallback;
    // Orca serialises a relative value as "50%" - percent of the default.
    if (s.find('%') != std::string::npos)
        return h.default_acceleration * v / 100.0;
    return v;
}

static nlohmann::json accel_xy(double v)
{
    return nlohmann::json{{"x", v}, {"y", v}};
}

static nlohmann::json build_accel_overrides(const HeaderData& h,
                                            size_t extruder_count)
{
    const double def   = h.default_acceleration;
    const double trav  = h.travel_acceleration > 0.0 ? h.travel_acceleration : def;

    // Jerk is optional. Cura writes max_speed_change_mm_per_s only when
    // jerk_enabled is set; without a value in the profile we write nothing
    // rather than invent a number.
    const double jerk_def = accel_from(h, "default_jerk", 0.0);
    const bool   has_jerk = jerk_def > 0.0;

    struct Rolle { const char* tag; const char* accel_key; const char* jerk_key; };
    static const Rolle kRollen[] = {
        {"WALL_OUTER",        "outer_wall_acceleration",             "outer_wall_jerk"},
        {"WALL_INNER",        "inner_wall_acceleration",             "inner_wall_jerk"},
        {"FILL",              "sparse_infill_acceleration",          "infill_jerk"},
        {"TOP_SURFACE",       "top_surface_acceleration",            "top_surface_jerk"},
        {"SUPPORT",           "support_acceleration",                nullptr},
        {"SUPPORT_INTERFACE", "support_acceleration",                nullptr},
        {"PRIME_TOWER",       nullptr,                               nullptr},
        {"SKIRT",             nullptr,                               nullptr},
    };

    nlohmann::json bead = nlohmann::json::object();
    bead["Travel Move"]["rate_mm_per_s_sq"] = accel_xy(trav);
    if (has_jerk)
        bead["Travel Move"]["max_speed_change_mm_per_s"] =
            accel_xy(accel_from(h, "travel_jerk", jerk_def));

    const size_t n = extruder_count < 1 ? 1 : extruder_count;
    for (size_t i = 0; i < n; ++i) {
        for (const Rolle& r : kRollen) {
            const std::string tag = std::string(r.tag) + "_" + std::to_string(i);
            const double a = r.accel_key ? accel_from(h, r.accel_key, def) : def;
            bead[tag]["rate_mm_per_s_sq"] = accel_xy(a);
            if (has_jerk) {
                const double j = r.jerk_key ? accel_from(h, r.jerk_key, jerk_def)
                                            : jerk_def;
                bead[tag]["max_speed_change_mm_per_s"] = accel_xy(j);
            }
        }
    }

    nlohmann::json ov;
    ov["rate_mm_per_s_sq"] = accel_xy(def);
    if (has_jerk)
        ov["max_speed_change_mm_per_s"] = accel_xy(jerk_def);
    ov["bead_mode"] = bead;
    return ov;
}

// A print job identifier. Cura writes print_information.slice_uuid here and
// MakerBot Print writes one too; the field is a plain RFC 4122 version 4
// string, so a locally generated one serves the same purpose.
static std::string make_print_uuid()
{
    static const char* kHex = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { out.push_back('-'); continue; }
        if (i == 14) { out.push_back('4'); continue; }              // version 4
        unsigned v = rd() & 0xF;
        if (i == 19) v = (v & 0x3) | 0x8;                           // variant
        out.push_back(kHex[v]);
    }
    return out;
}

// Half extents of the build volume around its centre, in the order Cura
// writes them: [right, front, left, back]. The reference file gives
// [205.0, 160.0, -205.0, -160.0] for the Method XL, whose plate is
// 410 x 320 mm.
static nlohmann::json machine_bounds(const PrintConfig& config)
{
    // The physical plate, when the machine profile states it. Cura's own
    // Method X file reports [141.65, 118.24, -141.65, -118.24] = 283.3 x
    // 236.48, which is machine_width x machine_depth of
    // ultimaker_method_base - NOT its printable area of 152 x 190. The two
    // differ wherever a machine has disallowed zones.
    if (const auto* ps_opt = config.option("makerbot_plate_size")) {
        const auto* ps = dynamic_cast<const ConfigOptionFloats*>(ps_opt);
        if (ps && ps->values.size() >= 2) {
            const double w = ps->values[0] / 2.0;
            const double d = ps->values[1] / 2.0;
            if (w > 5.0 && d > 5.0)
                return nlohmann::json::array({w, d, -w, -d});
        }
    }
    // No plate stated: fall back to the usable area, which is what this
    // function did before. Correct wherever plate and usable area coincide -
    // the whole Sketch series, which has no disallowed zones.
    const auto* pa_opt = config.option("printable_area");
    if (!pa_opt)
        return nullptr;
    try {
        const auto* pts = dynamic_cast<const ConfigOptionPoints*>(pa_opt);
        if (!pts || pts->values.size() < 2)
            return nullptr;
        double xmin = 1e9, xmax = -1e9, ymin = 1e9, ymax = -1e9;
        for (const auto& p : pts->values) {
            xmin = std::min(xmin, p.x()); xmax = std::max(xmax, p.x());
            ymin = std::min(ymin, p.y()); ymax = std::max(ymax, p.y());
        }
        const double w = (xmax - xmin) / 2.0;
        const double d = (ymax - ymin) / 2.0;
        if (w <= 5.0 || d <= 5.0)
            return nullptr;
        return nlohmann::json::array({w, d, -w, -d});
    } catch (...) {
        return nullptr;
    }
}

static nlohmann::json build_lava_meta(
    const PrintConfig&  config,
    const std::string&  bot_type,
    const HeaderData&   h,
    double              total_extrusion,
    const std::string&  project_name,
    const ToolpathStats* tp = nullptr)
{
    const std::string mat_lo  = lava_material_id(h.filament_type);
    const double      mass_g  = extrusion_mass_g(total_extrusion, h.filament_diameter, h.filament_density);

    // Determine tool types for Method/Sketch (mk14 / mk14_s)
    std::vector<std::string> tools;
    {
        const auto* opt = config.option("smart_extruder_type");
        if (opt) {
            try {
                const auto* ss = dynamic_cast<const ConfigOptionStrings*>(opt);
                if (ss) for (const auto& v : ss->values)
                    if (!v.empty() && v != "none") tools.push_back(v);
            } catch (...) {}
        }
    }
    std::string lava_bot = bot_type;
    if (lava_bot.empty()) {
        const auto* opt = config.option("printer_model");
        if (opt) try { lava_bot = dynamic_cast<const ConfigOptionString*>(opt)->value; } catch (...) {}
        if (lava_bot.empty()) lava_bot = "method";
    }

    // Only reached when the printer preset carries no smart_extruder_type.
    // This function serves the Method series and the Sketch series, and
    // MakerBot Print declares sketch.supported_extruders = sketch_extruder -
    // so a single hard-coded mk14 names an extruder the machine does not
    // have. The printer refuses such a job while preparing it, which is why
    // the fallback follows the machine and reports that it fired.
    if (tools.empty()) {
        const bool is_sketch =
            boost::algorithm::to_lower_copy(lava_bot).find("sketch") != std::string::npos;
        tools.push_back(is_sketch ? "sketch_extruder" : "mk14");
        BOOST_LOG_TRIVIAL(warning)
            << "MakerBotExport: no smart_extruder_type for bot_type '" << lava_bot
            << "', falling back to '" << tools.front() << "'";
    }

    nlohmann::json meta;
    meta["bot_type"]                = lava_bot;
    meta["commanded_duration_s"]    = h.duration_s;
    meta["duration_s"]              = h.duration_s;
    // One entry per extruder, not first-layer-and-rest of a single one.
    //
    // Measured on MakerBot Traffic Cone_ABSR_1h9m.makerbot (Method X,
    // ABS-R on 1XA, SR-30 on 2XA): the toolpath correctly commands 260 for
    // tool 0 and 255 for tool 1, while meta.json read [260, 260]. The old
    // expression paired first_layer_temp with temperature - two layers of
    // the same extruder - which happens to look right whenever a material's
    // first layer matches its other layers.
    //
    // Cura's reference file writes extruder_temperature = extruder 0 and
    // extruder_temperatures = one per extruder, taken from the print
    // temperature rather than the first-layer one.
    //
    // This is the header half of the fault B24/B27 fixed in the toolpath.
    {
        std::vector<int> lava_extruder_temperatures;
        const auto* opt = config.option("nozzle_temperature");
        if (opt) {
            try {
                const auto* ints = dynamic_cast<const ConfigOptionInts*>(opt);
                if (ints) for (int v : ints->values)
                    lava_extruder_temperatures.push_back(v);
            } catch (...) {}
        }
        if (lava_extruder_temperatures.empty())
            lava_extruder_temperatures.push_back(h.temperature);
        // Mirror tool_types and materials in length.
        while (lava_extruder_temperatures.size() < tools.size())
            lava_extruder_temperatures.push_back(lava_extruder_temperatures.back());
        while (lava_extruder_temperatures.size() > tools.size() &&
               lava_extruder_temperatures.size() > 1)
            lava_extruder_temperatures.pop_back();
        meta["extruder_temperature"]  = lava_extruder_temperatures.front();
        meta["extruder_temperatures"] = lava_extruder_temperatures;
    }
    // Per-extruder figures. MakerBot Print writes extrusion_distances_mm and
    // extrusion_masses_g as one entry PER EXTRUDER; the reference file
    // method_dual_test.makerbot carries [950.633, 1104.029] there, matching the
    // 'a' and 'b' axis sums of the toolpath exactly. The firmware reads these
    // fields (see birdwing_makerbot_extrakte_befunde.md) and answers a mismatch
    // with print_extruder_mismatch, so they must not stay single-valued on a
    // dual-material job.
    if (tp && tp->dual) {
        const double m0 = extrusion_mass_g(tp->extrusion[0], h.filament_diameter, h.filament_density);
        const double m1 = extrusion_mass_g(tp->extrusion[1], h.filament_diameter, h.filament_density);
        meta["extrusion_distance_mm"]  = tp->extrusion[0] + tp->extrusion[1];
        meta["extrusion_distances_mm"] = nlohmann::json::array({tp->extrusion[0], tp->extrusion[1]});
        meta["extrusion_mass_g"]       = m0 + m1;
        meta["extrusion_masses_g"]     = nlohmann::json::array({m0, m1});
    } else {
        meta["extrusion_distance_mm"]  = total_extrusion;
        meta["extrusion_distances_mm"] = nlohmann::json::array({total_extrusion});
        meta["extrusion_mass_g"]       = mass_g;
        meta["extrusion_masses_g"]     = nlohmann::json::array({mass_g});
    }
    meta["material"]                = mat_lo;
    // materials mirrors tool_types: one material name per configured extruder.
    {
        nlohmann::json mats = nlohmann::json::array();
        const auto* opt = config.option("filament_type");
        if (opt) {
            try {
                const auto* ss = dynamic_cast<const ConfigOptionStrings*>(opt);
                if (ss) for (const auto& v : ss->values)
                    // v is already a single entry here (ConfigOptionStrings),
                    // but a profile may still carry a joined string - filter
                    // it the same way rather than trusting the container.
                    mats.push_back(lava_material_id(first_filament_type(v)));
            } catch (...) {}
        }
        if (mats.empty()) mats.push_back(mat_lo);
        while (mats.size() < tools.size()) mats.push_back(mats.back());
        while (mats.size() > tools.size() && mats.size() > 1) mats.erase(mats.end() - 1);
        meta["materials"] = mats;
    }
    meta["model_counts"]            = nlohmann::json::array({nlohmann::json{{"count",1},{"name","instance0"}}});
    meta["name"]                    = project_name;
    meta["platform_temperature"]    = h.bed_temperature; // was: hardcoded 0
    meta["build_plane_temperature"] = h.chamber_temp;
    meta["tool_type"]               = tools.front();
    meta["tool_types"]              = tools;
    meta["version"]                 = "3.0.0";
    meta["uuid"]                    = make_print_uuid();
    // Cura writes the intent CATEGORY here, and the default one is literally
    // "default" - the reference file says so. "Balanced" is only its display
    // name and lives in slicemetadata.json under quality.intent_name.
    meta["preferences"]["instance0"]["printMode"] = "default";
    meta["preferences"]["instance0"]["machineBounds"] = machine_bounds(config);
    // Cura leaves the bounding box out for the Sketch series (it sets
    // bounds = None for application/x-makerbot-sketch) and writes it for the
    // Method series. tp is null exactly in the Sketch case, so the same rule
    // falls out of the existing branching.
    if (tp && tp->has_bbox) {
        BBox bb;
        bb.update(tp->min_x, tp->min_y, tp->min_z);
        bb.update(tp->max_x, tp->max_y, tp->max_z);
        meta["bounding_box"] = bb.to_json();
    }
    if (tp)
        meta["accel_overrides"] = build_accel_overrides(h, tools.size());
    meta["miracle_config"]["_bot"] = lava_bot;
    meta["miracle_config"]["_extruders"] = tools;
    meta["miracle_config"]["_materials"] = meta["materials"];
    meta["miracle_config"]["gaggles"]["instance0"] = nlohmann::json::object();
    return meta;
}

// Base64 decoder for thumbnail extraction
static std::string base64_decode(const std::string& input)
{
    static constexpr unsigned char kDec[256] = {
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,52,53,54,55,56,57,58,59,60,61,64,64,64,65,64,64,
        64,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
        64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64
    };
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (std::isspace(c)) continue;
        if (c == '=') break;
        const unsigned char d = kDec[c];
        if (d >= 64) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static std::vector<ThumbnailBlob> extract_thumbnails(const std::string& gcode_path)
{
    std::vector<ThumbnailBlob> out;
    std::ifstream gf(gcode_path);
    if (!gf.is_open()) return out;

    const std::regex begin_re(R"(^;\s*thumbnail\s+begin\s+(\d+)x(\d+)\s+\d+)", std::regex::icase);
    const std::regex end_re  (R"(^;\s*thumbnail\s+end)", std::regex::icase);

    bool collecting = false;
    int w = 0, h = 0;
    std::string b64;
    std::string line;

    while (std::getline(gf, line)) {
        std::smatch m;
        if (!collecting && std::regex_search(line, m, begin_re)) {
            collecting = true;
            w = parse_int_safe(m[1].str(), 0);
            h = parse_int_safe(m[2].str(), 0);
            b64.clear();
            continue;
        }
        if (collecting && std::regex_search(line, end_re)) {
            const std::string bytes = base64_decode(b64);
            if (!bytes.empty()) out.push_back({w, h, bytes});
            collecting = false;
            continue;
        }
        if (collecting) {
            std::string s = line;
            boost::algorithm::trim(s);
            if (!s.empty() && s[0] == ';') s.erase(s.begin());
            boost::algorithm::trim(s);
            b64 += s;
        }
    }
    return out;
}

static const ThumbnailBlob* choose_thumbnail(const std::vector<ThumbnailBlob>& thumbs, int w, int h)
{
    const ThumbnailBlob* best = nullptr;
    for (const auto& t : thumbs) {
        if (t.width == w && t.height == h) return &t;
        if (!best || (t.width * t.height) > (best->width * best->height)) best = &t;
    }
    return best;
}

static void add_thumbnail_entries(
    std::vector<std::pair<std::string, std::string>>& entries,
    const std::vector<ThumbnailBlob>& thumbs,
    const std::vector<std::pair<std::string, std::pair<int,int>>>& targets)
{
    for (const auto& tgt : targets)
        if (const ThumbnailBlob* t = choose_thumbnail(thumbs, tgt.second.first, tgt.second.second))
            entries.emplace_back(tgt.first, t->bytes);
}

static bool write_zip(const std::string& out_path,
                      const std::vector<std::pair<std::string, std::string>>& entries)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, out_path.c_str(), 0)) return false;
    for (const auto& e : entries)
        if (!mz_zip_writer_add_mem(&zip, e.first.c_str(), e.second.data(), e.second.size(), MZ_DEFAULT_COMPRESSION))
        { mz_zip_writer_end(&zip); return false; }
    const bool ok = mz_zip_writer_finalize_archive(&zip) != 0;
    mz_zip_writer_end(&zip);
    return ok;
}

// ── pack_makerbot_birdwing (Birdwing / 5th Gen) ─────────────────────────────

static bool pack_makerbot_birdwing(const std::string& gcode_path,
                                    const std::string& archive_path,
                                    const PrintConfig& config,
                                    const std::string& project_name)
{
    // 1. Parse ALL settings from G-code header + count filament in one pass
    const HeaderData header = parse_header(gcode_path, config);
    const std::vector<ThumbnailBlob> thumbnails = extract_thumbnails(gcode_path);

    // 2. Convert G-code to Birdwing JSON toolpath (Print 4.x format, relative.a=true)
    BirdwingBuildVolume bv;
    {
        const auto* pa_opt = config.option("printable_area");
        if (pa_opt) {
            try {
                const auto* pts = dynamic_cast<const ConfigOptionPoints*>(pa_opt);
                if (pts && pts->values.size() >= 2) {
                    const auto& vv = pts->values;
                    double xmin=1e9, xmax=-1e9, ymin=1e9, ymax=-1e9;
                    for (const auto& p : vv) {
                        xmin = std::min(xmin, p.x()); xmax = std::max(xmax, p.x());
                        ymin = std::min(ymin, p.y()); ymax = std::max(ymax, p.y());
                    }
                    // printable_area is in millimetres, not micrometres.
                    // MakerBotCoords::bed_centre() reads the very same points
                    // without dividing and sanity-checks against 1.0 mm. The
                    // division here turned a 300 mm plate into 0.3, the guard
                    // below rejected it, and bv silently kept its default -
                    // so the printer profile never reached the toolpath at
                    // all. bv.x/2 is the origin offset of every coordinate in
                    // it, so the print landed off-centre on any machine whose
                    // plate differs from the default.
                    const double W = xmax - xmin;
                    const double H = ymax - ymin;
                    if (W > 10.0 && H > 10.0) { bv.x = W; bv.y = H; }
                }
            } catch (...) {}
        }
    }
    bv.layer_width = header.line_width > 0.01 ? header.line_width : 0.4;

    std::string tp_error;
    ToolpathStats tp_stats;
    const std::string toolpath_json = gcode_to_birdwing_jsontoolpath(
        gcode_path, bv, header.layer_height, tp_error, &tp_stats);

    if (toolpath_json.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MakerBotExport: toolpath conversion failed: " << tp_error;
        return false;
    }

    // 3. Count commands for meta.json
    int command_count = 0;
    {
        // Quick count: every occurrence of "\"command\"" = one command
        const std::string needle = "\"command\"";
        size_t pos = 0;
        while ((pos = toolpath_json.find(needle, pos)) != std::string::npos) {
            ++command_count; pos += needle.size();
        }
    }

    // 4. Determine bot_type from config
    std::string bot_type = "z18_6";
    {
        const auto* bt = config.option("makerbot_bot_type");
        if (bt) try { bot_type = dynamic_cast<const ConfigOptionString*>(bt)->value; } catch (...) {}
    }

    // 5. Build meta.json using header (all values from G-code settings block)
    // bounding_box is one of the fields the firmware actually reads. It has to
    // describe the PRINTED OBJECT, not the machine - the build volume was only
    // ever a placeholder here (measured on a Z18 cube: 300 x 305 reported for a
    // 30 mm part). The converter now returns the extent of the extruding moves.
    BBox bbox;
    if (tp_stats.has_bbox) {
        bbox.update(tp_stats.min_x, tp_stats.min_y, tp_stats.min_z);
        bbox.update(tp_stats.max_x, tp_stats.max_y, tp_stats.max_z);
    } else {
        // No extrusion found (empty plate) - fall back to the build volume so
        // the field stays well-formed.
        bbox.update(-bv.x/2, -bv.y/2, 0.0);
        bbox.update( bv.x/2,  bv.y/2, header.layer_height * header.num_layers);
    }

    const nlohmann::json meta = build_birdwing_meta(
        config, bot_type, header, bbox,
        header.total_filament_mm, command_count, project_name);

    // 6. Pack ZIP archive
    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("meta.json",          meta.dump(4));
    entries.emplace_back("print.jsontoolpath", toolpath_json);
    add_thumbnail_entries(entries, thumbnails, {
        {"thumbnail_320x200.png",          {320,  200}},
        {"thumbnail_110x80.png",           {110,   80}},
        {"thumbnail_55x40.png",            { 55,   40}},
        {"isometric_thumbnail_640x640.png",{640,  640}},
        {"isometric_thumbnail_320x320.png",{320,  320}},
        {"isometric_thumbnail_120x120.png",{120,  120}}
    });

    if (!write_zip(archive_path, entries)) {
        BOOST_LOG_TRIVIAL(error) << "MakerBotExport: failed to write archive: " << archive_path;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "MakerBotExport: Birdwing archive created: " << archive_path
        << " [" << header.duration_s << "s, "
        << header.total_filament_mm << "mm, "
        << header.outer_wall_speed << "mm/s outer, "
        << header.retraction_length << "mm retract]";
    return true;
}

// ── pack_makerbot_lava (Method / Sketch = Lava format) ───────────────────────

static bool pack_makerbot_lava(const std::string& gcode_path,
                                const std::string& archive_path,
                                const PrintConfig& config,
                                const std::string& project_name)
{
    std::string gcode;
    {
        std::ifstream f(gcode_path, std::ios::binary);
        if (!f.is_open()) return false;
        gcode.assign(std::istreambuf_iterator<char>(f), {});
    }

    const HeaderData header = parse_header(gcode_path, config);
    const std::vector<ThumbnailBlob> thumbnails = extract_thumbnails(gcode_path);

    std::string bot_type;
    {
        const auto* bt = config.option("makerbot_bot_type");
        if (bt) try { bot_type = dynamic_cast<const ConfigOptionString*>(bt)->value; } catch (...) {}
    }

    // ── Sketch vs. Method: two different archive payloads ─────────────────
    // Cura's MakerbotWriter (plugins/MakerbotWriter/MakerbotWriter.py) branches
    // on the machine's file_formats MIME type:
    //   application/x-makerbot-sketch  -> print.gcode
    //   application/x-makerbot         -> print.jsontoolpath   (Method series)
    //   ...-replicator_plus            -> print.jsontoolpath
    // Both groups share meta version 3.0.0, so the version does NOT identify
    // the payload. The fork routed Method and Sketch through this one packer
    // and shipped raw G-code for both; MakerBot Print's own Method export
    // (method_dual_test.makerbot, bot lava_f) contains print.jsontoolpath.
    // Ultimaker owns MakerBot - Cura is the reference here.
    std::string model_name;
    {
        const auto* opt = config.option("printer_model");
        if (opt) try { model_name = dynamic_cast<const ConfigOptionString*>(opt)->value; } catch (...) {}
    }
    const bool is_sketch =
        boost::algorithm::to_lower_copy(bot_type).find("sketch") != std::string::npos ||
        boost::algorithm::to_lower_copy(model_name).find("sketch") != std::string::npos;

    // Sketch executes print.gcode as-is, and MakerBot firmware puts the origin
    // in the middle of the platform - Cura states it per machine as
    // machine_center_is_zero: true (ultimaker_sketch*.def.json). Orca hands us
    // corner coordinates, so translate before packing.
    // The Method branch below needs nothing: gcode_to_birdwing_jsontoolpath
    // already subtracts the bed centre while building the toolpath.
    if (is_sketch) {
        const MakerBotCoords::BedCentre centre = MakerBotCoords::bed_centre(config);
        if (!centre.valid) {
            BOOST_LOG_TRIVIAL(error)
                << "MakerBotExport: printer profile has no usable printable_area - "
                   "refusing to pack print.gcode in corner coordinates";
            return false;
        }
        gcode = MakerBotCoords::to_machine_coordinates(gcode, centre);
        BOOST_LOG_TRIVIAL(info)
            << "MakerBotExport: print.gcode bed corner -> machine origin, shifted by "
            << -centre.x << " / " << -centre.y << " mm";
    }

    std::string toolpath_json;
    ToolpathStats tp_stats;
    if (!is_sketch) {
        // Same derivation as the Birdwing packer: the toolpath origin is the
        // bed centre, so the build volume must come from printable_area.
        BirdwingBuildVolume bv;
        bv.x = 152.0;  // Method fallback
        bv.y = 190.0;
        {
            const auto* pa_opt = config.option("printable_area");
            if (pa_opt) {
                try {
                    const auto* pts = dynamic_cast<const ConfigOptionPoints*>(pa_opt);
                    if (pts && pts->values.size() >= 2) {
                        double xmin=1e9, xmax=-1e9, ymin=1e9, ymax=-1e9;
                        for (const auto& p : pts->values) {
                            xmin = std::min(xmin, p.x()); xmax = std::max(xmax, p.x());
                            ymin = std::min(ymin, p.y()); ymax = std::max(ymax, p.y());
                        }
                        // Same fix as in the Birdwing packer above:
                        // printable_area is in millimetres. With the division
                        // the Method XL used the Method X's 152 x 190 default
                        // and every print landed 76.5 / 57.5 mm off centre.
                        const double W = xmax - xmin;
                        const double H = ymax - ymin;
                        if (W > 10.0 && H > 10.0) { bv.x = W; bv.y = H; }
                    }
                } catch (...) {}
            }
        }
        bv.layer_width = header.line_width > 0.01 ? header.line_width : 0.4;

        std::string tp_error;
        toolpath_json = gcode_to_birdwing_jsontoolpath(
            gcode_path, bv, header.layer_height, tp_error, &tp_stats,
            /*lava_format=*/true);
        if (toolpath_json.empty()) {
            BOOST_LOG_TRIVIAL(error)
                << "MakerBotExport: Lava toolpath conversion failed: " << tp_error;
            return false;
        }
    }

    const nlohmann::json meta = build_lava_meta(
        config, bot_type, header, header.total_filament_mm, project_name,
        is_sketch ? nullptr : &tp_stats);

    nlohmann::json slicemeta;
    slicemeta["generator"] = "OrcaSlicer MakerBot Lava native export";

    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("meta.json",          meta.dump(4));
    if (is_sketch)
        entries.emplace_back("print.gcode",        gcode);
    else
        entries.emplace_back("print.jsontoolpath", toolpath_json);
    entries.emplace_back("slicemetadata.json", slicemeta.dump(4));
    add_thumbnail_entries(entries, thumbnails, {
        {"thumbnail_140x106.png",          {140,  106}},
        {"thumbnail_212x300.png",          {212,  300}},
        {"thumbnail_960x1460.png",         {960, 1460}},
        {"thumbnail_90x90.png",            { 90,   90}},
        {"isometric_thumbnail_120x120.png",{120,  120}},
        {"isometric_thumbnail_320x320.png",{320,  320}},
        {"isometric_thumbnail_640x640.png",{640,  640}}
    });

    if (!write_zip(archive_path, entries)) {
        BOOST_LOG_TRIVIAL(error) << "MakerBotExport: failed to write Lava archive: " << archive_path;
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "MakerBotExport: Lava archive created: " << archive_path;
    return true;
}

// ── Public entry point ───────────────────────────────────────────────────────

std::string pack_to_archive(const std::string& gcode_path, const PrintConfig& config)
{
    namespace fs = boost::filesystem;

    const GCodeFlavor flavor = config.gcode_flavor;

    // Determine archive output path
    const std::string ext = get_archive_extension(flavor);
    if (ext == ".gcode")
        return {}; // MakerBotLegacy: leave as plain G-code

    const fs::path gcode_p(gcode_path);
    const std::string archive_path = (gcode_p.parent_path() / (gcode_p.stem().string() + ext)).string();

    // BUG FIX (2026-06-19): when the export dialog (Plater.cpp) already names
    // the target with the final archive extension (.makerbot), gcode_path and
    // the freshly computed archive_path collapse onto the SAME file (stem()
    // strips exactly one extension, so "Cube.makerbot" -> stem "Cube" ->
    // archive_path "Cube.makerbot" again). The packer then overwrites that
    // path with the real ZIP archive, and the unconditional cleanup further
    // down used to call fs::remove(gcode_path) on that identical path right
    // afterwards - silently destroying the just-written archive (the
    // try/catch swallows everything, so nothing shows up in the log;
    // confirmed by reproducing with debug logging: "Birdwing archive
    // created" is logged, then the file is gone, with zero trace of the
    // removal itself). Mirrors the fix already applied in
    // GPXExport::pack_to_archive for the x3g/Legacy pipeline.
    //
    // project_name is captured from archive_path (the real, final, visible
    // filename) BEFORE any staging happens, rather than re-derived from
    // gcode_path inside each packer. That decouples the on-printer/on-screen
    // project title from the staging implementation entirely.
    const std::string project_name = fs::path(archive_path).stem().string();

    std::string gcode_source = gcode_path;
    bool used_temp_source = false;
    if (archive_path == gcode_path) {
        gcode_source = gcode_path + ".tmp_gcode_for_pack";
        try {
            fs::rename(gcode_path, gcode_source);
            used_temp_source = true;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "MakerBotExport: could not stage temp G-code for "
                << gcode_path << ": " << e.what();
            return {};
        }
    }

    bool ok = false;
    if (flavor == gcfMakerBotLava) {
        ok = pack_makerbot_lava(gcode_source, archive_path, config, project_name);
    } else {
        // gcfMakerBotBirdwing (and anything else → try Birdwing)
        ok = pack_makerbot_birdwing(gcode_source, archive_path, config, project_name);
    }

    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "MakerBotExport: packing failed for " << gcode_source;
        if (used_temp_source) {
            try { fs::rename(gcode_source, gcode_path); } catch (...) {}
        }
        return {};
    }

    // Diagnostics: keep the intermediate G-code next to the archive when
    // ORCA_MAKERBOT_KEEP_GCODE is set. Mirrors ORCA_GPX_KEEP_GCODE in
    // GPXExport.cpp. Chasing a conversion bug means comparing the input and
    // the output of the SAME run - exporting them separately gives two slices
    // that may differ in filament or process preset without anyone noticing.
    //
    // gcode_source is GUARANTEED to differ from archive_path here - either it
    // was already a distinct path, or it is our temp staging file - so neither
    // branch can touch the archive we just wrote.
    if (! getenv_string("ORCA_MAKERBOT_KEEP_GCODE").empty()) {
        const std::string kept = archive_path + ".gcode";
        try {
            fs::rename(gcode_source, kept);
            BOOST_LOG_TRIVIAL(info) << "MakerBotExport: intermediate G-code kept at " << kept;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "MakerBotExport: could not keep intermediate G-code: " << e.what();
            try { fs::remove(gcode_source); } catch (...) {}
        }
    } else {
        try { fs::remove(gcode_source); } catch (...) {}
    }

    BOOST_LOG_TRIVIAL(info) << "MakerBotExport: archive at " << archive_path;
    return archive_path;
}

} // namespace MakerBotExport
} // namespace Slic3r
