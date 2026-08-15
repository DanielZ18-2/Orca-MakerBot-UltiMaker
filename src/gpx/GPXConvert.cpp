#include "GPXConvert.hpp"

extern "C" {
#include "gpx.h"
}

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace Slic3r {
namespace GPX {

namespace {

// GPX keeps a handful of file-scope tables and, more importantly, was written
// as a single-shot CLI. Orca can export several plates at once, so serialise.
std::mutex g_gpx_mutex;

// RAII for the three FILE* the conversion needs.
struct FileGuard
{
    FILE* f { nullptr };
    explicit FileGuard(FILE* file = nullptr) : f(file) {}
    ~FileGuard() { if (f) std::fclose(f); }
    FileGuard(const FileGuard&)            = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    explicit operator bool() const { return f != nullptr; }
};

// GPX writes its diagnostics to gpx->log as plain text. Route that into a
// temp file so a failed conversion can report WHY it failed instead of just
// an exit code - that was the weak point of the old std::system() call.
std::string drain(FILE* f)
{
    if (!f)
        return {};
    std::string out;
    std::fflush(f);
    if (std::fseek(f, 0L, SEEK_SET) != 0)
        return {};
    char   buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    // Keep error dialogs readable - the log can run to thousands of lines when
    // GPX moans about every unsupported M-code.
    constexpr size_t max_len = 8000;
    if (out.size() > max_len)
        out = out.substr(0, max_len) + "\n[... GPX log truncated ...]";
    return out;
}

std::string basename_no_ext(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    std::string leaf = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = leaf.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        leaf = leaf.substr(0, dot);
    return leaf;
}

// gpx_set_property() and gpx_start_convert() take char*, not const char*.
std::vector<char> mutable_copy(const std::string& s)
{
    std::vector<char> v(s.begin(), s.end());
    v.push_back('\0');
    return v;
}

} // namespace

bool convert_gcode_to_x3g(const std::string& gcode_path,
                          const std::string& x3g_path,
                          const std::string& machine,
                          const std::string& build_name,
                          const std::string& ini_path,
                          std::string*       error,
                          ConvertStats*      stats)
{
    auto fail = [&](const std::string& msg, FILE* log) {
        if (error) {
            *error = msg;
            const std::string tail = drain(log);
            if (!tail.empty())
                *error += "\nGPX log:\n" + tail;
        }
        return false;
    };

    if (!is_known_machine(machine))
        return fail("GPX does not know the machine code '" + machine +
                    "'. Known codes:\n" + known_machines(), nullptr);

    std::lock_guard<std::mutex> lock(g_gpx_mutex);

    FileGuard in(std::fopen(gcode_path.c_str(), "r"));
    if (!in)
        return fail("GPX: cannot open input G-code: " + gcode_path, nullptr);

    FileGuard out(std::fopen(x3g_path.c_str(), "wb"));
    if (!out)
        return fail("GPX: cannot create output file: " + x3g_path, nullptr);

    // tmpfile() can fail on locked-down Windows accounts; fall back to no log
    // capture rather than to stderr, which would spam the console.
    FileGuard log(std::tmpfile());

    // Gpx is a large struct (tens of KB of buffers) - keep it off the stack.
    auto gpx = std::make_unique<Gpx>();
    gpx_initialize(gpx.get(), 1);
    gpx->log = log ? log.f : nullptr;
    // verboseMode makes GPX name the offending line number when it rejects a
    // command; that is what makes the log worth capturing at all.
    gpx->flag.verboseMode = 1;

    // NOTE: the default ~/.gpx.ini / gpx.ini lookup the CLI performs is
    // deliberately NOT reproduced. A stray ini in the user's home directory
    // must not silently change what Orca writes to the printer. An explicit
    // ini is still honoured - see ORCA_GPX_INI in GPXExport.cpp.
    if (!ini_path.empty()) {
        const int rc = gpx_load_config(gpx.get(), ini_path.c_str());
        if (rc < 0)
            return fail("GPX: cannot load config file: " + ini_path, log.f);
        if (rc > 0)
            return fail("GPX: syntax error in config file " + ini_path +
                        " at line " + std::to_string(rc), log.f);
    }

    {
        std::vector<char> m = mutable_copy(machine);
        if (gpx_set_property(gpx.get(), "printer", "machine_type", m.data()))
            return fail("GPX: rejected machine type '" + machine + "'", log.f);
    }

    std::string name = build_name.empty() ? basename_no_ext(x3g_path) : build_name;
    if (name.empty())
        name = "OrcaSlicer";
    std::vector<char> name_buf = mutable_copy(name);

    gpx_start_convert(gpx.get(), name_buf.data(), 0, 0);
    const int rval = gpx_convert(gpx.get(), in.f, out.f, nullptr);
    gpx_end_convert(gpx.get());

    if (rval != SUCCESS)
        return fail("GPX conversion failed (code " + std::to_string(rval) + ")", log.f);

    if (stats) {
        stats->filament_mm = gpx->total.length;
        stats->duration_s  = gpx->total.time;
        stats->bytes       = static_cast<double>(gpx->total.bytes);
    }

    // Flush before the guard closes it, so a full disk surfaces here and not
    // as a silently truncated .x3g the printer chokes on halfway through.
    if (std::fflush(out.f) != 0 || std::ferror(out.f))
        return fail("GPX: error writing " + x3g_path, log.f);

    return true;
}

bool is_known_machine(const std::string& machine)
{
    if (machine.empty())
        return false;
    std::lock_guard<std::mutex> lock(g_gpx_mutex);
    return gpx_find_machine(machine.c_str()) != nullptr;
}

std::string known_machines()
{
    std::lock_guard<std::mutex> lock(g_gpx_mutex);
    FileGuard tmp(std::tmpfile());
    if (!tmp)
        return {};
    gpx_list_machines(tmp.f);
    return drain(tmp.f);
}

std::string version()
{
    return PACKAGE_VERSION;
}

} // namespace GPX
} // namespace Slic3r
