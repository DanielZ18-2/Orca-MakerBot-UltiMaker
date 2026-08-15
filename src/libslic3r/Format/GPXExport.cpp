#include "GPXExport.hpp"

#include "libslic3r/PrintConfig.hpp"

// Embedded GPX (markwal/GPX 2.6.8), see src/gpx.
#include "GPXConvert.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace {

std::string getenv_string(const char* name)
{
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

// Generic option access via the shared ConfigBase interface -
// works identically for PrintConfig (StaticConfig) and DynamicPrintConfig
// (DynamicConfig), unlike .has()/.opt_string(), which only exist on
// DynamicConfig.
std::string config_string_if_present(const PrintConfig& config, const std::string& key)
{
    if (const auto* opt = config.option(key))
        if (const auto* s = dynamic_cast<const ConfigOptionString*>(opt))
            return s->value;
    return {};
}

std::string printer_model_string(const PrintConfig& config)
{
    std::string model = config_string_if_present(config, "printer_model");
    if (model.empty())
        model = config_string_if_present(config, "model_id");
    if (model.empty())
        model = config_string_if_present(config, "printer_notes");
    return model;
}

// Number of configured extruders (nozzle-diameter array length) - used to
// distinguish single-/dual-extruder within the same product line
// (e.g. Replicator 1 single vs. dual, TOM single vs. dual).
int extruder_count(const PrintConfig& config)
{
    if (const auto* opt = config.option("nozzle_diameter"))
        if (const auto* v = dynamic_cast<const ConfigOptionFloats*>(opt))
            return std::max<int>(1, static_cast<int>(v->values.size()));
    return 1;
}

bool contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

// ── GPX-Maschinen-Zuordnung ──────────────────────────────────────────────────
//
// The short codes used here are taken 1:1 from the actual GPX source
// and cross-checked against the official MakerBot Desktop "bot_type" values
// (sources: github.com/markwal/GPX, src/shared/std_machines.h - now embedded
// at src/gpx/shared/std_machines.h; as well as
// Library/MakerBot/default_configs/*.json from the official
// MakerBot-Print-Installation):
//
//   bot_type (MakerBot)      Achsen/Tools (offiziell)        GPX -m
//   -----------------------  -------------------------------  ------
//   tomstepstrudersingle     1 Tool (Mk7), X106 Y120 Z106      t7   (t6 is identical)
//   (TOM, 2 Tools)           2 Tools                            t7d
//   replicatorsingle         1 Tool (Mk8/A),  X225 Y145 Z150    r1
//   replicatordual           2 Tools (Mk8/A+B)                  r1d
//   replicator2              1 Tool (Mk8/A)                     r2
//   replicator2x             2 Tools (Mk8/A+B), X246 Y152 Z155  r2x
//
// Cupcake was never officially listed by MakerBot Desktop/Print (the device
// predates that software) - the c3/c4/cp4/cpp codes come directly from
// GPX itself (Gen3/Gen4/Pololu electronics variants).
std::string GPXExport::gpx_machine_for_config(const PrintConfig& config)
{
    // Optional future profile key. It is intentionally read defensively so old
    // profiles still load if the key does not exist in PrintConfig yet.
    if (std::string explicit_machine = config_string_if_present(config, "gpx_machine_type"); !explicit_machine.empty()) {
        if (GPX::is_known_machine(explicit_machine))
            return explicit_machine;
        BOOST_LOG_TRIVIAL(warning) << "GPXExport: profile requests unknown GPX machine '" << explicit_machine
            << "', falling back to model detection. Known codes:\n" << GPX::known_machines();
    }

    const std::string model = boost::algorithm::to_lower_copy(printer_model_string(config));
    const int extruders = extruder_count(config);

    // Replicator 2X (always 2 extruders, own code independent of extruders)
    if (contains(model, "2x"))
        return "r2x";

    // Replicator 2 (non-X). "hbp"/"heated" -> heated-bed mod.
    if (contains(model, "replicator 2") || contains(model, "replicator2")) {
        if (contains(model, "hbp") || contains(model, "heated"))
            return "r2h";
        return "r2";
    }

    // Replicator 1 / "Original" (single or dual depending on extruder count)
    if (contains(model, "replicator")) {
        if (contains(model, "dual") || extruders >= 2)
            return "r1d";
        return "r1";
    }

    // Thing-O-Matic
    if (contains(model, "thing-o-matic") || contains(model, "thing o matic") || contains(model, "tom")) {
        if (extruders >= 2)
            return "t7d";
        if (contains(model, "mk6"))
            return "t6";
        return "t7"; // Mk7 - and mechanically identical to Mk6, thus an uncritical default
    }

    // Cupcake-Varianten (Elektronik/Extruder-Generation)
    if (contains(model, "cupcake")) {
        if (contains(model, "pololu") && (contains(model, "gen4") || contains(model, "mk5") || contains(model, "mk6")))
            return "cp4";
        if (contains(model, "pololu"))
            return "cpp";
        if (contains(model, "gen4") || contains(model, "g4"))
            return "c4";
        return "c3"; // Gen3 - most common/oldest Cupcake revision
    }

    // No model name recognized: safe fallback to the dual-legacy machine
    // most common in this project.
    BOOST_LOG_TRIVIAL(warning) << "GPXExport: could not identify printer_model '" << model
        << "' for GPX machine mapping, falling back to r2x. Set 'gpx_machine_type' explicitly to override.";
    return "r2x";
}

bool GPXExport::export_to_x3g(
    const std::string& gcode_filepath,
    const std::string& output_filepath,
    const PrintConfig& config,
    std::string* error_message)
{
    auto fail = [&](const std::string& msg) {
        BOOST_LOG_TRIVIAL(error) << "GPXExport: " << msg;
        if (error_message) *error_message = msg;
        return false;
    };

    if (!fs::exists(gcode_filepath))
        return fail("Input G-code file does not exist: " + gcode_filepath);

    const std::string machine = gpx_machine_for_config(config);

    fs::path out_path(output_filepath);
    if (!out_path.parent_path().empty()) {
        boost::system::error_code ec;
        fs::create_directories(out_path.parent_path(), ec);
    }

    // Optional developer override: a tuned GPX .ini without exposing it as an
    // Orca post script. The library does NOT read ~/.gpx.ini on its own - a
    // stray ini must not silently change what we send to a printer.
    const std::string ini_path = getenv_string("ORCA_GPX_INI");

    // NOTE on G-code flavour: the library leaves GPX at reprapFlavor = 1, and
    // the CLI's "-g" (MakerBot/ReplicatorG flavour) is deliberately NOT
    // reproduced. With -g, GPX mis-reads three things (gpx.c, markwal/GPX):
    //
    //   * M106/M107 (gpx.c:5313/5356): reprap flavor routes them to set_valve()
    //     - the blower output the Replicator drives its PART cooling fan from.
    //     With -g they go to set_fan() instead, the extruder HEATSINK fan. The
    //     part fan then never turns on: overhangs droop, bridges sag.
    //   * T0/T1 (gpx.c:4627): "Makerbot Tn is not sticky" - with -g the tool
    //     selection reverts to the current extruder after every command, so
    //     Orca's sticky tool changes are effectively ignored. Dual-material on
    //     Replicator 2X / Original Dual would print entirely from one extruder.
    //   * M109 (gpx.c:5438): takes the non-reprap path, ignoring the T
    //     parameter for the wait.
    //
    // GPX defaults to reprapFlavor = 1 (gpx.c:360), which is what Orca output
    // needs.
    BOOST_LOG_TRIVIAL(info) << "GPXExport: converting " << gcode_filepath
        << " -> " << output_filepath << " (GPX " << GPX::version() << ", machine " << machine << ")";

    std::string error;
    GPX::ConvertStats stats;
    // Empty build name -> derived from the .x3g file name, which is what the
    // user sees on the printer's LCD.
    if (!GPX::convert_gcode_to_x3g(gcode_filepath, output_filepath, machine,
                                   /* build_name */ {}, ini_path, &error, &stats)) {
        // Do not leave a half-written .x3g behind - the firmware would happily
        // start printing it and stop mid-object.
        boost::system::error_code ec;
        fs::remove(output_filepath, ec);
        return fail(error);
    }

    if (!fs::exists(output_filepath))
        return fail("GPX reported success but did not create output file: " + output_filepath);

    BOOST_LOG_TRIVIAL(info) << "GPXExport: " << output_filepath << " written, "
        << stats.filament_mm << " mm filament, " << stats.duration_s << " s estimated";

    return true;
}

// ── Dispatch-friendly wrapper ────────────────────────────────────────────────

std::string GPXExport::get_archive_extension(GCodeFlavor flavor)
{
    return flavor == gcfMakerBotLegacy ? ".x3g" : ".gcode";
}

std::string GPXExport::pack_to_archive(const std::string& gcode_path, const PrintConfig& config)
{
    if (config.gcode_flavor != gcfMakerBotLegacy)
        return {}; // not our flavor - nothing to do

    const fs::path gcode_p(gcode_path);

    // BUG FIX (2026-06-18): Since the export dialog (Plater.cpp) now names the
    // final path with .x3g directly, `gcode_path` here is frequently ALREADY
    // the desired final .x3g path - but its CONTENT is still plain G-code text
    // (finalize_gcode()'s copy_file() doesn't care about extensions). The old
    // code below derived archive_path by stripping and re-appending ".x3g",
    // which collapses to the SAME path as gcode_path in that case - gpx would
    // be asked to read and write the identical file, and the cleanup
    // fs::remove(gcode_path) afterwards then deleted the just-created result.
    // Confirmed via real export: "Exported successfully" toast shown, but no
    // file left on disk. Fix: if gcode_path already ends in .x3g, stage the
    // G-code content aside to a sibling temp file first, convert FROM there
    // TO the real target path, then remove the temp file - never collapse
    // input and output into the same path.
    std::string archive_path;
    std::string gcode_source = gcode_path;
    bool        used_temp_source = false;

    if (gcode_p.extension() == ".x3g") {
        archive_path = gcode_path;
        gcode_source = gcode_path + ".tmp_gcode_for_gpx";
        try {
            fs::rename(gcode_path, gcode_source);
            used_temp_source = true;
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "GPXExport: could not stage temp G-code for " << gcode_path << ": " << e.what();
            return {};
        }
    } else {
        archive_path = (gcode_p.parent_path() / (gcode_p.stem().string() + ".x3g")).string();
    }

    std::string error;
    if (!export_to_x3g(gcode_source, archive_path, config, &error)) {
        BOOST_LOG_TRIVIAL(warning) << "GPXExport: failed for " << gcode_source << ": " << error;
        // Best effort: give the user back their G-code instead of leaving nothing.
        if (used_temp_source) {
            try { fs::rename(gcode_source, gcode_path); } catch (...) {}
        }
        return {};
    }

    try { fs::remove(gcode_source); } catch (...) {}

    BOOST_LOG_TRIVIAL(info) << "GPXExport: x3g archive created: " << archive_path;
    return archive_path;
}

} // namespace Slic3r
