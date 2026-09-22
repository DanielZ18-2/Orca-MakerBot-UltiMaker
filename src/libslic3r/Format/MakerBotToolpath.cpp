// MakerBot / UltiMaker Fork - Orca Slicer 2.4
// MakerBotToolpath.cpp  - FULLY REWORKED 2026-06-14
//
// G-code -> Birdwing JSON Toolpath converter.
// Basiert auf Reverse-Engineering des 1cm_x_1cm_block_Rep+.makerbot
// aus MakerBot Print 4.10.1 (Resources.zip/app.asar.unpacked/)
//
// CRITICAL FIXES compared to the previous version:
//  1. JSON-Struktur:   Jeder Befehl in {"command":{...}} eingebettet
//  2. Metadata field:   {"relative":{"a":true,"x":false,"y":false,"z":false}}
//  3. Tag-Namen:       "Travel Move","Retract","Restart","Inset","Infill",
//                      "Trailing Extrusion Move","Leaky Travel Move","Connection"
//  4. Retract-Moves:   Tag="Retract", a=negative mm (MK13: -0.5, MK12: -1.0)
//  5. Restart-Moves:   Tag="Restart", a=positive mm (~0.6)
//  6. Z tracking:       cur_z always updated (even before in_print_area)
//  7. Comments:         "Layer Section N (M)" instead of "chunk N (N)"
//  8. Feedrate:        F/60 (mm/min->mm/s) taken correctly from the G-code
//  9. Koordinaten:     full float precision (no integer rounding)

#include "MakerBotToolpath.hpp"
#include "libslic3r/LocalesUtils.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <clocale>
#include <locale>
#include <algorithm>

#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>

namespace Slic3r {

// -- Locale-independent double parsing --------------------------------------
// Same bug / same fix as in MakerBotExport.cpp::parse_double_safe:
// std::stod follows the global C locale (setlocale), which
// wxWidgets typically sets the system locale at startup. Under
// a comma-decimal locale (e.g. de_DE), dot values like "123.45"
// silently misinterpreted from the G-code. This function ALWAYS parses per
// the "C" convention (dot as decimal separator), independent of the
// process locale - this affects EVERY X/Y/Z/E coordinate and every
// line-width value in the real toolpath sent to the printer.
static bool parse_double_locale_safe(const std::string& s, double& out)
{
    // Locale-unabhaengig via Upstream-Helfer (fast_float, immer '.'). Der Helfer
    // leaves out uninitialized on failure and returns pos==0 -> check pos.
    size_t pos = 0;
    const double v = string_to_double_decimal_point(s, &pos);
    if (pos == 0 || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// Defense-in-depth, identical to MakerBotExport.cpp (see there for details):
// forces LC_NUMERIC="C" for the duration of the scope, then restores the
// previous state, so it does not affect the rest of the GUI.
class ScopedCNumericLocale
{
public:
    ScopedCNumericLocale()
    {
        const char* cur = std::setlocale(LC_NUMERIC, nullptr);
        m_prev = cur ? cur : "C";
        std::setlocale(LC_NUMERIC, "C");
    }
    ~ScopedCNumericLocale() { std::setlocale(LC_NUMERIC, m_prev.c_str()); }
    ScopedCNumericLocale(const ScopedCNumericLocale&) = delete;
    ScopedCNumericLocale& operator=(const ScopedCNumericLocale&) = delete;
private:
    std::string m_prev;
};

// -- Tag-Mapping: Orca ;TYPE: -> Birdwing JSON-Tag ----------------------------
// Referenz: 1cm_x_1cm_block_Rep+.makerbot aus MakerBot Print 4.10.1
// Valid tags: "Trailing Extrusion Move", "Infill", "Inset",
//               "Leaky Travel Move", "Travel Move", "Connection",
//               "Retract", "Restart", "Support", "Outline"
static std::string orca_type_to_birdwing_tag(const std::string& orca_type)
{
    // "Outline" = outer wall (MakerBot Desktop 3.x / Birdwing)
    // "Inset"   = Innenwand
    if (orca_type == "Outer wall")             return "Outline";
    if (orca_type == "Inner wall")             return "Inset";
    if (orca_type == "Sparse infill")          return "Infill";
    if (orca_type == "Internal solid infill")  return "Infill";
    if (orca_type == "Top surface")            return "Infill";
    if (orca_type == "Bottom surface")         return "Infill";
    if (orca_type == "Bridge")                 return "Bridge";
    if (orca_type == "Internal Bridge")        return "Bridge";
    if (orca_type == "Support")                return "Support";
    if (orca_type == "Support interface")      return "Support";
    if (orca_type == "Overhang wall")          return "Outline";
    if (orca_type == "Skirt")                  return "Outline";
    if (orca_type == "Brim")                   return "Outline";
    if (orca_type == "Support transition")     return "Support";
    if (orca_type == "Custom")                 return ""; // start/end gcode -> skip
    if (orca_type == "Ironing")                return "Infill";
    return "Infill"; // sicherer Default
}

// -- G-code-Parameter parsen (z.B. "X123.45" -> 123.45) -----------------------
static bool parse_gcode_param(const std::string& token, char axis, double& out)
{
    if (token.empty() || std::toupper(token[0]) != std::toupper(axis))
        return false;
    return parse_double_locale_safe(token.substr(1), out);
}

// -- Dual-Extruder-Modus ------------------------------------------------------
// Set for the duration of one conversion. In dual mode every move carries BOTH
// extruder axes ("a" and "b"), the inactive one at 0.0, and metadata.relative
// lists "b" as well. Verified against method_dual_test.makerbot (MakerBot
// Print, bot lava_f): all 6108 moves carry a and b, never both non-zero, and
// the per-axis sums match meta.json's extrusion_distances_mm exactly.
// File-local rather than a parameter because make_command() is called from a
// dozen places; one conversion runs start to finish before the next begins.
static bool g_dual_axes = false;

// Lava (Method) writes format 3.0.0, Birdwing 1.2.0. The one structural
// difference inside the toolpath: in 3.0.0 every NON-move command carries an
// empty "metadata" object, while 1.2.0 repeats the relative block everywhere.
// Verified against method_dual_test.makerbot (all 111 set_toolhead_temperature,
// 100 toggle_fan, 191 fan_duty and 1129 comment entries have "metadata": {}).
static bool g_lava_format = false;

// -- Befehl als {"command":{...}} emittieren -----------------------------------
// CRITICAL: MakerBot Print ALWAYS expects the outer "command" wrapper!
static nlohmann::json make_command(
    const std::string& function_name,
    const nlohmann::json& parameters,
    const std::vector<std::string>& tags,
    bool relative_a = true)
{
    nlohmann::json cmd;
    cmd["function"] = function_name;
    if (g_lava_format && function_name != "move") {
        cmd["metadata"] = nlohmann::json::object();
    } else {
        nlohmann::json rel = {
            {"a", relative_a},
            {"x", false},
            {"y", false},
            {"z", false}
        };
        if (g_dual_axes) rel["b"] = relative_a;
        cmd["metadata"] = {{"relative", rel}};
    }
    cmd["parameters"] = parameters;
    cmd["tags"] = tags;
    return {{"command", cmd}};
}

// Move parameters with the extrusion put on the axis of the active tool.
// Single-extruder output is unchanged (only "a"), so Birdwing files stay
// byte-identical to before.
static nlohmann::json move_params(
    double x, double y, double z, double e, double feedrate, int tool)
{
    nlohmann::json p = {{"x", x}, {"y", y}, {"z", z}};
    if (g_dual_axes) {
        p["a"] = (tool == 0) ? e : 0.0;
        p["b"] = (tool == 1) ? e : 0.0;
    } else {
        p["a"] = e;
    }
    p["feedrate"] = feedrate;
    return p;
}

// The empty command that MakerBot Print emits immediately before every
// change_toolhead - present in all 49 tool changes of the reference file.
static nlohmann::json make_empty_command()
{
    nlohmann::json cmd;
    cmd["metadata"]   = nlohmann::json::object();
    cmd["parameters"] = nlohmann::json::object();
    cmd["tags"]       = nlohmann::json::array();
    return {{"command", cmd}};
}

static nlohmann::json make_comment(const std::string& text)
{
    return make_command("comment",
        {{"comment", text}},
        nlohmann::json::array(),
        false);
}

// -- Layer-Kommentarblock emittieren ------------------------------------------
// Referenz-Format aus 1cm_x_1cm_block_Rep+.makerbot:
//   "Layer Section 0 (1)"
//   "Material 0"
//   "Lower Position  0"
//   "Upper Position  0.3"
//   "Thickness       0.3"
//   "Width           2.5"
static void emit_layer_comments(
    nlohmann::json& commands,
    int    layer_idx,     // 0-basiert
    double z_upper,
    double z_lower,
    double thickness,
    double width)
{
    auto fmt = [](double v) -> std::string {
        // MakerBot uses no trailing-zero format - direct string conversion
        std::ostringstream o;
        o << v;
        return o.str();
    };
    // Layer-Index 0-basiert, Display-Nummer 1-basiert (in Klammern)
    commands.push_back(make_comment(
        "Layer Section " + std::to_string(layer_idx) + " (" +
        std::to_string(layer_idx + 1) + ")"));
    commands.push_back(make_comment("Material 0"));
    // Indented fields with consistent width as in the original
    commands.push_back(make_comment("Lower Position  " + fmt(z_lower)));
    commands.push_back(make_comment("Upper Position  " + fmt(z_upper)));
    commands.push_back(make_comment("Thickness       " + fmt(thickness)));
    commands.push_back(make_comment("Width           " + fmt(width)));
}

// ==============================================================================
// HAUPT-KONVERTER: G-code -> Birdwing JSON Toolpath
// ==============================================================================
std::string gcode_to_birdwing_jsontoolpath(
    const std::string&        gcode_path,
    const BirdwingBuildVolume& bv,
    double                    layer_height,
    std::string&              error,
    ToolpathStats*            stats,
    bool                      lava_format)
{
    const ScopedCNumericLocale locale_guard; // see the comment at the class definition
    g_lava_format = lava_format;

    // -- Pre-pass: does this job switch tools at all? --------------------------
    // Only a real switch to T1+ makes it a dual job. Orca emits a single "T0"
    // even for single-material prints on a dual machine, which must stay
    // single-axis output.
    // -- Per-extruder settings from the G-code footer ---------------------------
    // Without them tool_temp[1] stays at -1 on dual jobs and the
    // entering extruder never reaches its print temperature. Measured on
    // 3DBenchy_ABS_3h9m.makerbot (Method X, ABS + PETG): 75x standby
    // temperature 180 for tool 1, not once the configured 240, across
    // 95893 extrusion moves on the b axis.
    std::vector<double> cfg_nozzle_temp;         // nozzle_temperature = 245,240
    std::vector<double> cfg_nozzle_temp_first;   // nozzle_temperature_initial_layer
    std::vector<double> cfg_idle_temp;           // idle_temperature, coInts per extruder
    double              cfg_standby_delta = -5.0;

    auto parse_num_list = [](const std::string& s) {
        std::vector<double> out;
        std::string cur;
        for (char c : s) {
            if (c == ',' || c == ';') {
                if (!cur.empty()) out.push_back(std::atof(cur.c_str()));
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) out.push_back(std::atof(cur.c_str()));
        return out;
    };


    bool dual_job = false;
    // Highest tool index the job addresses. The toolpath format has two
    // extruder axes, so anything above 1 cannot be represented.
    int max_tool_seen = 0;
    {
        std::ifstream pre(gcode_path);
        if (!pre.is_open()) {
            error = "Cannot open: " + gcode_path;
            return "";
        }
        std::string l;
        while (std::getline(pre, l)) {
            const size_t s = l.find(';');
            std::string body = (s != std::string::npos) ? l.substr(0, s) : l;
            boost::trim(body);

            // Einstellungsblock am Dateiende: nur reine Kommentarzeilen
            if (s == 0) {
                std::string cline = l.substr(1);
                boost::trim(cline);
                const size_t eqp = cline.find('=');
                if (eqp != std::string::npos) {
                    std::string ckey = cline.substr(0, eqp);
                    std::string cval = cline.substr(eqp + 1);
                    boost::trim(ckey);
                    boost::trim(cval);
                    if (ckey == "nozzle_temperature")
                        cfg_nozzle_temp = parse_num_list(cval);
                    else if (ckey == "nozzle_temperature_initial_layer")
                        cfg_nozzle_temp_first = parse_num_list(cval);
                    else if (ckey == "idle_temperature")
                        cfg_idle_temp = parse_num_list(cval);
                    else if (ckey == "standby_temperature_delta")
                        cfg_standby_delta = std::atof(cval.c_str());
                }
            }
            if (body.size() >= 2 && (body[0] == 'T' || body[0] == 't') &&
                std::isdigit(static_cast<unsigned char>(body[1]))) {
                const int tn = std::atoi(body.c_str() + 1);
                if (tn != 0) dual_job = true;
                if (tn > max_tool_seen) max_tool_seen = tn;
            }
        }
    }
    g_dual_axes = dual_job;

    // A tool index beyond 1 would silently lose all its extrusion:
    // make_move_params() puts 0.0 on both axes for an unknown tool, so the
    // moves are written as travels and the object prints as air. Refuse.
    if (max_tool_seen > 1) {
        error = "This job uses extruder T" + std::to_string(max_tool_seen) +
                ", but the MakerBot toolpath format supports two extruders "
                "(T0 and T1) only. A project file may have brought more "
                "filaments than this printer has extruders. Load the objects "
                "individually and assign filaments 1 and 2 only.";
        return "";
    }

    std::ifstream f(gcode_path);
    if (!f.is_open()) {
        error = "Cannot open: " + gcode_path;
        return "";
    }

    // Coordinate offset: Orca origin (left-front-bottom) ->
    // MakerBot-Ursprung (Mitte des Druckbetts)
    const double x_offset = bv.x / 2.0;   // Z18: 150.0
    const double y_offset = bv.y / 2.0;   // Z18: 152.5

    nlohmann::json commands = nlohmann::json::array();

    // -- Parser state ------------------------------------------------------
    // Orca ALWAYS uses relative E (use_relative_e_distances=1 for gcfMakerBotBirdwing)
    // MakerBot G-code sends no M82/M83 - absolute_ext = false is fixed.
    const bool absolute_ext = false;
    bool       absolute_pos = true;

    double cur_x = 0, cur_y = 0, cur_z = 0, cur_e = 0;
    double cur_feedrate = 40.0; // mm/s Default
    double layer_z_lower = 0.0;
    int    layer_idx     = -1;
    bool   in_print_area = false;
    bool   in_custom_block = false;
    bool   retracted     = false;

    // True as soon as ONE G1 with an E parameter has been seen. Orca emits an E
    // value for every extruding move, so from that point on "G1 with XY but
    // without E" is unambiguously a travel move. The geometry-based fallback
    // below must never fire in that case - it would turn every travel into an
    // extruding print move (measured: 15.8 % of all extrusion, 1652 mm3, on a
    // Z18 temperature tower).
    bool   gcode_has_e   = false;

    // -- Dual-extruder state --------------------------------------------------
    // active_tool addresses the axis: 0 -> "a", 1 -> "b".
    // pending_tool holds a tool change until the next XY move is known: the
    // reference file puts the purge-wall start point into change_toolhead's
    // x/y, and that is exactly where the first move after the change goes.
    // Deriving it instead of keeping a per-machine table avoids inventing
    // coordinates the head would then travel to.
    int    active_tool   = 0;
    int    pending_tool  = -1;
    int    toolchanges   = 0;
    double sum_axis[2]   = {0.0, 0.0};
    // Bounding box of the printed object - only extruding moves count, so
    // travels, purge lines and the brim-free approach do not inflate it.
    double bb_min_x = 1e30, bb_max_x = -1e30;
    double bb_min_y = 1e30, bb_max_y = -1e30;
    double bb_min_z = 1e30, bb_max_z = -1e30;
    bool   bb_valid = false;
    // How much filament is currently retracted on each axis (positive = pulled
    // back). Orca retracts the OUTGOING extruder by retraction_length plus
    // retract_length_toolchange (default 10 mm) before a "Tn" and only primes it
    // again when that extruder comes back. Without tracking this, the incoming
    // extruder starts its first path with an empty melt zone - measured on a
    // Method X dual export: 6 x 10 mm never returned, i.e. centimetres of
    // missing extrusion after every change.
    double axis_retracted[2] = {0.0, 0.0};
    // Per-tool target temperature, filled from M104/M109 (with or without T).
    double tool_temp[2]  = {-1.0, -1.0};

    // Seeded from the settings block. M104/M109 override these later
    // where present - but not for tool 1, because the start G-code
    // only heats T0.
    for (int ti = 0; ti < 2; ++ti) {
        if (static_cast<int>(cfg_nozzle_temp.size()) > ti && cfg_nozzle_temp[ti] > 0.0)
            tool_temp[ti] = cfg_nozzle_temp[ti];
        else if (static_cast<int>(cfg_nozzle_temp_first.size()) > ti && cfg_nozzle_temp_first[ti] > 0.0)
            tool_temp[ti] = cfg_nozzle_temp_first[ti];
    }
    // Standby temperature of the idle extruder. 180 C in the reference file;
    // MakerBot uses a fixed standby, Orca has no matching setting.
    const int STANDBY_TEMP = 180;

    // Standby temperature of the idle extruder, in this order:
    //   1) idle_temperature[n], if the user set it
    //   2) 180 as the default - MakerBot's measured value; the entering
    //      extruder is waited for anyway (wait_for_temperature), so a
    //      cold standby costs time, not quality
    //   3) if the print temperature is BELOW 180 the default would be
    //      too hot; then print temperature plus standby_temperature_delta
    auto standby_for = [&](int tool) -> int {
        if (tool >= 0 && tool < 2) {
            if (static_cast<int>(cfg_idle_temp.size()) > tool && cfg_idle_temp[tool] > 0.0)
                return static_cast<int>(cfg_idle_temp[tool] + 0.5);
            if (tool_temp[tool] > 0.0 && tool_temp[tool] < static_cast<double>(STANDBY_TEMP))
                return static_cast<int>(tool_temp[tool] + cfg_standby_delta + 0.5);
        }
        return STANDBY_TEMP;
    };
    // Tool-change retract/prime: 1.0 mm at 5 mm/s, both directions, measured
    // over all 49 changes of the reference file (48 retracts, 49 primes).
    const double TOOLCHANGE_RETRACT = 1.0;
    const double TOOLCHANGE_FEEDRATE = 5.0;

    std::string current_tag = "Outline";
    double      layer_w     = bv.layer_width;

    // Fan and temperature state: emit only on change, otherwise
    // the toolpath bloats unnecessarily.
    double last_fan_value   = -1.0;   // 0.0-1.0, -1 = not set yet
    bool   fan_is_on        = false;
    double last_tool_temp   = -1.0;

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // -- Kommentare / Direktiven ------------------------------------------
        const size_t semi = line.find(';');
        const std::string body = (semi != std::string::npos)
                                 ? line.substr(0, semi) : line;
        const std::string comment = (semi != std::string::npos)
                                    ? line.substr(semi + 1) : "";

        // ;TYPE:xxx - classification of the following moves
        if (!comment.empty() && comment.find("TYPE:") == 0) {
            const std::string type = boost::trim_copy(comment.substr(5));
            const std::string new_tag = orca_type_to_birdwing_tag(type);
            if (new_tag.empty()) {
                // "Custom" -> start/end G-code -> Ausgabe pausieren
                in_print_area = false;
                in_custom_block = true;
            } else {
                current_tag   = new_tag;
                in_print_area = true;
                in_custom_block = false;
            }
            continue;
        }

        // ;WIDTH:xxx - Linienbreite
        if (!comment.empty() && comment.find("WIDTH:") == 0) {
            double w = 0.0;
            if (parse_double_locale_safe(boost::trim_copy(comment.substr(6)), w))
                layer_w = w;
            continue;
        }

        const std::string cmd_str = boost::trim_copy(body);
        if (cmd_str.empty()) continue;

        std::vector<std::string> tokens;
        {
            std::istringstream ss(cmd_str);
            std::string tok;
            while (ss >> tok) tokens.push_back(tok);
        }
        if (tokens.empty()) continue;
        const std::string& cmd = tokens[0];

        // -- Positioniermodus -------------------------------------------------
        if (cmd == "G90") { absolute_pos = true;  continue; }
        if (cmd == "G91") { absolute_pos = false; continue; }
        if (cmd == "M82") { /* absolute_ext = true  - ignored by Birdwing */ continue; }
        if (cmd == "M83") { /* absolute_ext = false - the Birdwing default */ continue; }

        // -- M106 / M107 - Bauteilluefter --------------------------------------
        // The firmware (libparser.so) knows fan_duty and toggle_fan; miracle_grue
        // uses both and tags them with "Fan Speed Change" / "Enable Fan" /
        // "Disable Fan". Without these commands the fan runs constantly at the
        // a value from meta.json.
        if (cmd == "M106" || cmd == "M107") {
            double duty = (cmd == "M107") ? 0.0 : 1.0;   // M106 without S = full power
            bool   has_s = false;
            if (cmd == "M106") {
                for (size_t i = 1; i < tokens.size(); ++i) {
                    double v = 0;
                    if (parse_gcode_param(tokens[i], 'S', v)) {
                        duty  = std::max(0.0, std::min(1.0, v / 255.0));
                        has_s = true;
                        break;
                    }
                }
            }
            (void) has_s;
            const bool want_on = duty > 0.0005;

            // An-/Ausschalten nur bei echtem Zustandswechsel
            if (want_on != fan_is_on) {
                commands.push_back(make_command("toggle_fan",
                    {{"index", 0}, {"value", want_on}},
                    { want_on ? std::string("Enable Fan") : std::string("Disable Fan") },
                    false));
                fan_is_on = want_on;
            }
            // Set speed only when the fan runs and the value changes
            if (want_on && std::abs(duty - last_fan_value) > 0.0005) {
                commands.push_back(make_command("fan_duty",
                    {{"index", 0}, {"value", duty}},
                    { std::string("Fan Speed Change") },
                    false));
                last_fan_value = duty;
            }
            if (!want_on) last_fan_value = 0.0;
            continue;
        }

        // -- M104 / M109 - Duesentemperatur ------------------------------------
        // M109 waits for temperature in Marlin; the Birdwing firmware knows
        // no wait_for_temperature (only Method/Lava), so both are
        // set_toolhead_temperature abgebildet.
        if (cmd == "M104" || cmd == "M109") {
            double t = -1.0;
            int    idx = -1;      // T parameter, if Orca addressed a tool
            for (size_t i = 1; i < tokens.size(); ++i) {
                double v = 0;
                if (parse_gcode_param(tokens[i], 'S', v)) t = v;
                if (parse_gcode_param(tokens[i], 'T', v)) idx = static_cast<int>(v);
            }
            const int target = (idx == 0 || idx == 1) ? idx : active_tool;
            if (t >= 0.0) {
                // Remember per tool - the tool-change sequence needs the target
                // temperature of the extruder being switched to.
                tool_temp[target] = t;
                // A temperature for the *other* extruder is only remembered, not
                // emitted: the change sequence sets it at the right moment.
                if (target == active_tool && std::abs(t - last_tool_temp) > 0.5) {
                    commands.push_back(make_command("set_toolhead_temperature",
                        {{"index", target}, {"temperature", static_cast<int>(t + 0.5)}},
                        nlohmann::json::array(),
                        false));
                    last_tool_temp = t;
                }
            }
            continue;
        }

        // -- G28 - Alle Achsen homen -------------------------------------------
        if (cmd == "G28") {
            cur_x = 0; cur_y = 0; cur_z = 0;
            continue;
        }

        // -- G92 - Achsenposition setzen --------------------------------------
        if (cmd == "G92") {
            bool e_reset = false;
            for (size_t i = 1; i < tokens.size(); ++i) {
                double v = 0;
                if (parse_gcode_param(tokens[i], 'E', v)) { cur_e = v; e_reset = true; }
                if (parse_gcode_param(tokens[i], 'X', v)) cur_x = v;
                if (parse_gcode_param(tokens[i], 'Y', v)) cur_y = v;
                if (parse_gcode_param(tokens[i], 'Z', v)) cur_z = v;
            }
            if (e_reset) {
                // After G92 E0: reset retract state (critical for
                // correct E-tracking accumulation after the purge line)
                retracted = false;
            }
            continue;
        }

        // -- Tn - tool change: not converted yet -------------------------------
        // The Birdwing/Lava toolpath addresses the second extruder through its
        // own axis ("b" instead of "a"); meta.json mirrors that with
        // extrusion_distances_mm as a two-element array. That mapping is not
        // implemented yet and must not be guessed - a wrong toolpath would
        // drive the head into an undefined state.
        //
        // Refuse the export instead of silently producing a file in which the
        // whole job runs off extruder 0. Affects the four Lava machines
        // (Method, Method X, Method XL, Method X CF). Single-material jobs on
        // those machines are unaffected: Orca emits "T0" once and no switch.
        //
        // Legacy machines (Replicator 2X, Original Dual) do NOT come through
        // here - they export via GPX, which translates T0/T1 itself.
        if (!cmd.empty() && (cmd[0] == 'T' || cmd[0] == 't') && cmd.size() >= 2 &&
            std::isdigit(static_cast<unsigned char>(cmd[1]))) {
            const int tool_id = std::atoi(cmd.c_str() + 1);
            if (tool_id < 0 || tool_id > 1) {
                error = "The MakerBot toolpath format addresses two extruders "
                        "(axes a and b); the G-code selects T" +
                        std::to_string(tool_id) + ".";
                BOOST_LOG_TRIVIAL(error) << "MakerBotToolpath: " << error;
                return {};
            }
            // Defer: the change is emitted at the next XY move, whose
            // coordinates go into change_toolhead.
            if (tool_id != active_tool) pending_tool = tool_id;
            else                        pending_tool = -1;
            continue;
        }

        // -- G2 / G3 - arc moves: not supported --------------------------------
        // The Birdwing toolpath format knows only linear moves. Silently
        // dropping arcs would leave holes in the part, so refuse the export
        // instead. Guard against a user enabling "Arc fitting" in the UI - the
        // MakerBot profiles ship it disabled.
        if (cmd == "G2" || cmd == "G3") {
            error = "Arc fitting (G2/G3) is not supported by the MakerBot "
                    "toolpath format. Disable 'Arc fitting' in Quality > "
                    "Precision and slice again.";
            BOOST_LOG_TRIVIAL(error) << "MakerBotToolpath: " << error;
            return {};
        }

        // -- G1 / G0 - Bewegungsbefehl -----------------------------------------
        if (cmd == "G1" || cmd == "G0") {
            double nx = cur_x, ny = cur_y, nz = cur_z, ne = cur_e, nf = -1.0;
            bool has_x = false, has_y = false, has_z = false, has_e = false;

            for (size_t i = 1; i < tokens.size(); ++i) {
                double v = 0;
                if (parse_gcode_param(tokens[i], 'X', v)) {
                    nx = absolute_pos ? v : cur_x + v; has_x = true;
                }
                if (parse_gcode_param(tokens[i], 'Y', v)) {
                    ny = absolute_pos ? v : cur_y + v; has_y = true;
                }
                if (parse_gcode_param(tokens[i], 'Z', v)) {
                    nz = absolute_pos ? v : cur_z + v; has_z = true;
                }
                if (parse_gcode_param(tokens[i], 'E', v)) {
                    // Orca Birdwing: IMMER relativ (absolute_ext=false)
                    // -> ne = cur_e + e_raw, e_delta = e_raw
                    ne = absolute_ext ? v : cur_e + v;
                    has_e = true;
                    gcode_has_e = true;
                }
                if (parse_gcode_param(tokens[i], 'F', v)) {
                    nf = v / 60.0; // mm/min -> mm/s
                }
            }

            // Update feedrate if specified
            if (nf > 0) cur_feedrate = nf;

            // Remember previous position - BEFORE the position update, so
            // the distance calculation for the a-value computation below is correct.
            const double prev_x = cur_x;
            const double prev_y = cur_y;

            // ALWAYS update Z (even before in_print_area!)
            if (has_z) {
                // Layer-Wechsel: neues Z-Maximum erreicht (monoton steigend).
                // Z-hops (up then down) are correctly ignored, because after the
                // hop the Z falls back to the previous level.
                // FIX: no fragile tolerance check anymore - only Z > last Z-max.
                if (in_print_area && nz > layer_z_lower + 1e-5) {
                    ++layer_idx;
                    const double thickness = nz - layer_z_lower;
                    const double width = (layer_w > 1e-5) ? layer_w : 0.4; // Fallback
                    emit_layer_comments(commands, layer_idx,
                        nz,           // upper = new Z
                        layer_z_lower,// lower = vorheriges Z-Maximum
                        thickness,
                        width);
                    layer_z_lower = nz;
                }
                cur_z = nz;
            }

            // update position and E state
            cur_x = nx; cur_y = ny; cur_e = ne;

            // Before the print area: emit no moves
            if (!in_print_area && !in_custom_block) continue;

            // No XY move -> pure retract / unretract / Z-hop -> handled separately
            const bool has_xy = has_x || has_y;

            // -- Deferred tool change ------------------------------------------
            // Emitted here, at the first XY move after "Tn", because
            // change_toolhead carries the coordinates the head moves to next.
            // Sequence and values taken 1:1 from method_dual_test.makerbot
            // (49 changes, all identical in structure).
            if (pending_tool >= 0 && has_xy && in_print_area) {
                const int old_tool = active_tool;
                const int new_tool = pending_tool;
                const double tx = nx - x_offset;
                const double ty = ny - y_offset;

                // 1) long retract on the outgoing extruder
                commands.push_back(make_command("move",
                    move_params(cur_x - x_offset, cur_y - y_offset, cur_z,
                                -TOOLCHANGE_RETRACT, TOOLCHANGE_FEEDRATE, old_tool),
                    {"Long Retract"}));
                sum_axis[old_tool] -= TOOLCHANGE_RETRACT;
                axis_retracted[old_tool] += TOOLCHANGE_RETRACT;

                // 2) outgoing extruder to standby, its fan on
                commands.push_back(make_command("set_toolhead_temperature",
                    {{"index", old_tool}, {"temperature", standby_for(old_tool)}},
                    nlohmann::json::array(), false));
                commands.push_back(make_command("toggle_fan",
                    {{"index", old_tool}, {"value", true}},
                    nlohmann::json::array(), false));
                commands.push_back(make_command("fan_duty",
                    {{"index", old_tool}, {"value", 1.0}},
                    nlohmann::json::array(), false));

                // 3) incoming extruder to its printing temperature
                if (tool_temp[new_tool] >= 0.0) {
                    commands.push_back(make_command("set_toolhead_temperature",
                        {{"index", new_tool},
                         {"temperature", static_cast<int>(tool_temp[new_tool] + 0.5)}},
                        nlohmann::json::array(), false));
                    last_tool_temp = tool_temp[new_tool];
                }

                // 4) the empty command, then the change itself
                commands.push_back(make_empty_command());
                commands.push_back(make_command("change_toolhead",
                    {{"index", new_tool}, {"x", tx}, {"y", ty}},
                    nlohmann::json::array(), false));
                commands.push_back(make_command("wait_for_temperature",
                    {{"index", new_tool}}, nlohmann::json::array(), false));
                commands.push_back(make_command("delay",
                    {{"seconds", 5}}, nlohmann::json::array(), false));

                // 5) outgoing extruder's fan off again
                commands.push_back(make_command("toggle_fan",
                    {{"index", old_tool}, {"value", false}},
                    nlohmann::json::array(), false));

                active_tool = new_tool;
                pending_tool = -1;
                ++toolchanges;

                // 6) travel to the change position, then prime the new extruder
                commands.push_back(make_command("move",
                    move_params(tx, ty, cur_z, 0.0, 250.0, new_tool),
                    {"Travel Move"}));
                // Prime the incoming extruder by everything still retracted on its
                // axis - that includes Orca's retract_length_toolchange from the
                // previous switch. At minimum the reference value of 1.0 mm.
                const double prime = std::max(TOOLCHANGE_RETRACT, axis_retracted[new_tool]);
                commands.push_back(make_command("move",
                    move_params(tx, ty, cur_z, prime,
                                TOOLCHANGE_FEEDRATE, new_tool),
                    {"Long Restart"}));
                sum_axis[new_tool] += prime;
                axis_retracted[new_tool] = 0.0;

                // The filament of the incoming extruder is primed, so the
                // retract state of the outgoing one no longer applies.
                retracted = false;
            }

            // E-Delta (in relativem Modus = raw E-Wert direkt)
            // Denn: ne = cur_e_alt + e_raw -> e_delta = ne - cur_e_alt = e_raw
            const double e_raw = has_e ? [&]() -> double {
                for (size_t i = 1; i < tokens.size(); ++i) {
                    double v = 0;
                    if (parse_gcode_param(tokens[i], 'E', v)) return v;
                }
                return 0.0;
            }() : 0.0;

            // -- Pure retract move (no XY, negative E) -------------------
            if (!has_xy && has_e && e_raw < -1e-4) {
                // KORREKTES FORMAT: Tag="Retract", a=negative mm
                commands.push_back(make_command("move",
                    move_params(nx - x_offset, ny - y_offset, nz,
                                e_raw, cur_feedrate, active_tool),
                    {"Retract"}));
                sum_axis[active_tool] += e_raw;
                axis_retracted[active_tool] -= e_raw;   // e_raw < 0
                retracted = true;
                continue;
            }

            // -- Pure restart move (no XY, positive E after retract) -----
            if (!has_xy && has_e && e_raw > 1e-4 && retracted) {
                // KORREKTES FORMAT: Tag="Restart", a=positive mm
                commands.push_back(make_command("move",
                    move_params(nx - x_offset, ny - y_offset, nz,
                                e_raw, cur_feedrate, active_tool),
                    {"Restart"}));
                sum_axis[active_tool] += e_raw;
                axis_retracted[active_tool] = std::max(0.0, axis_retracted[active_tool] - e_raw);
                retracted = false;
                continue;
            }

            // -- Z-only move (no XY, no E) -> travel move -----------------
            if (!has_xy && !has_e) {
                if (in_custom_block) {
                    commands.push_back(make_command("move",
                        move_params(cur_x - x_offset, cur_y - y_offset, nz,
                                    0.0, cur_feedrate, active_tool),
                        {"Travel Move"}));
                }
                continue;
            }
            if (!has_xy && has_e && std::fabs(e_raw) < 1e-4) continue;

            // -- Koordinaten validieren ----------------------------------------
            const double json_x = nx - x_offset;
            const double json_y = ny - y_offset;
            if (json_x < -(bv.x / 2 + 10) || json_x > (bv.x / 2 + 10) ||
                json_y < -(bv.y / 2 + 10) || json_y > (bv.y / 2 + 10)) {
                BOOST_LOG_TRIVIAL(debug)
                    << "MakerBotToolpath: skip out-of-bounds x=" << json_x
                    << " y=" << json_y;
                continue;
            }

            // -- XY-Move klassifizieren ----------------------------------------
            std::string tag;

            if (e_raw > 1e-6) {
                // Positives E -> Extrusion
                if (retracted) {
                    tag = "Restart";
                    retracted = false;
                } else {
                    // A short extruding segment still belongs to its feature.
                    //
                    // This used to re-tag every extruding move below 0.5 mm as
                    // "Trailing Extrusion Move". That tag means something else
                    // to MakerBot: in their own export (method_dual_test.makerbot,
                    // bot lava_f) it carries 181 moves and not one of them
                    // extrudes - it is the trailing move AFTER extrusion stops.
                    //
                    // The heuristic cost the classification of 30271 of 79265
                    // extruding moves (38 %) on a Z18 tower. No material was
                    // lost - net extrusion matched meta.json exactly - but
                    // MiracleGrue drives speed and fan from the extrusion
                    // profile (insetsExtrusionProfile, infillsExtrusionProfile,
                    // outlinesExtrusionProfile ...), so an untagged move runs on
                    // defaults instead of its own profile.
                    //
                    // "Trailing Extrusion Move" stays reserved for moves that do
                    // NOT extrude; Orca marks those with ";TYPE:Wipe", which the
                    // tag table above already maps.
                    tag = current_tag;
                }
            } else if (e_raw < -1e-4) {
                // Negative E on an XY move -> retract during motion (rare)
                tag = "Retract";
                retracted = true;
            } else if (!has_e && has_xy && in_print_area && !gcode_has_e &&
                       current_tag != "Travel Move" &&
                       current_tag != "Leaky Travel Move") {
                // -- Fallback: G-code entirely WITHOUT inline E values ----------
                // Only reachable when not a single E parameter has appeared so
                // far. Extrusion is then derived from the geometry:
                //   a = L x layer_height x line_width / (pi x (d_fil/2)^2)
                //
                // WARNING - this branch used to run unconditionally. Orca DOES
                // emit E for every extruding move, and a travel move ("G1 X.. Y..
                // F9000" without E) kept the last print TYPE, so every travel was
                // written out as an extruding Infill/Inset/Outline move with a
                // computed a-value. Measured on a Z18 ABS tower: 5131 moves at
                // travel feedrate 150 mm/s carrying 671 mm of filament
                // (1652 mm3 = 15.8 % of all extrusion) - stringing across the
                // whole part. The reset of `retracted` below additionally caused
                // the following de-retraction to be tagged "Trailing Extrusion
                // Move" instead of "Restart".
                tag = current_tag;
                if (retracted) { tag = "Restart"; retracted = false; }
                // a is computed below
            } else if (!has_e && has_xy) {
                // -- Travel move: XY without E ----------------------------------
                // `retracted` is deliberately NOT cleared here - the filament is
                // still retracted during the travel and only comes back with the
                // following "G1 E+x", which must be tagged "Restart".
                tag = "Travel Move";
            } else {
                // No / minimal E + no print TAG -> Travel
                if (std::fabs(e_raw) < 1e-6) {
                    tag = "Travel Move";
                } else {
                    tag = "Leaky Travel Move";
                }
            }

            // -- a-Wert bestimmen --------------------------------------------------
            double a_val;
            if (has_e) {
                // E direkt aus G-code (relativ = delta)
                a_val = e_raw;
            } else if (!tag.empty() &&
                       tag != "Travel Move" &&
                       tag != "Leaky Travel Move") {
                // No E in the G-code -> compute from geometry
                // Formula: a = dist x (layer_h x line_w) / A_filament
                // A_filament = pi x (1.77/2)^2 = 2.4606 mm^2
                const double dx   = nx - prev_x;
                const double dy   = ny - prev_y;
                const double dist = std::sqrt(dx*dx + dy*dy);
                const double lh   = (layer_height > 1e-5) ? layer_height : 0.2;
                const double lw   = (layer_w     > 1e-5) ? layer_w     : 0.4;
                const double fil_area = 3.14159265358979 * 0.885 * 0.885; // (1.77/2)^2
                a_val = dist * lh * lw / fil_area;
            } else {
                a_val = 0.0;
            }

            // -- Move emittieren -----------------------------------------------
            // Retract has a negative a_val (e_raw) - must NOT be clipped!
            // For everything else: a should be >= 0.
            const double a_emit = (tag == "Retract") ? a_val : std::max(0.0, a_val);
            commands.push_back(make_command("move",
                move_params(json_x, json_y, nz, a_emit, cur_feedrate, active_tool),
                {tag}));
            sum_axis[active_tool] += a_emit;
            if (a_emit > 1e-9) {
                bb_min_x = std::min(bb_min_x, json_x); bb_max_x = std::max(bb_max_x, json_x);
                bb_min_y = std::min(bb_min_y, json_y); bb_max_y = std::max(bb_max_y, json_y);
                bb_min_z = std::min(bb_min_z, nz);     bb_max_z = std::max(bb_max_z, nz);
                bb_valid = true;
            }
        }
    }

    // End-of-print Kommentar
    commands.push_back(make_comment("End of print"));

    if (stats) {
        stats->extrusion[0] = sum_axis[0];
        stats->extrusion[1] = sum_axis[1];
        stats->tool_changes = toolchanges;
        stats->dual         = dual_job;
        stats->has_bbox     = bb_valid;
        if (bb_valid) {
            stats->min_x = bb_min_x; stats->max_x = bb_max_x;
            stats->min_y = bb_min_y; stats->max_y = bb_max_y;
            stats->min_z = bb_min_z; stats->max_z = bb_max_z;
        }
    }

    BOOST_LOG_TRIVIAL(info)
        << "MakerBotToolpath: " << commands.size()
        << " Befehle aus " << gcode_path
        << (dual_job ? (" (dual, " + std::to_string(toolchanges) +
                        " tool changes, a=" + std::to_string(sum_axis[0]) +
                        " b=" + std::to_string(sum_axis[1]) + ")")
                     : std::string());

    // Reset so a following conversion is not affected.
    g_dual_axes  = false;
    g_lava_format = false;

    return commands.dump();
}





} // namespace Slic3r
