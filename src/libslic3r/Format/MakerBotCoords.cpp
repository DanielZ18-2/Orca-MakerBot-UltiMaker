#include "MakerBotCoords.hpp"

#include "libslic3r/Config.hpp"

#include <algorithm>
#include <fstream>
#include <string>

namespace Slic3r {
namespace MakerBotCoords {
namespace {

// Everything below runs in fixed point, micrometres per unit.
//
// Parsing and formatting are hand-rolled on purpose. strtod() and
// snprintf("%f") honour LC_NUMERIC, and on a desktop with a decimal comma -
// German, French, Spanish, most of continental Europe - strtod stops at the
// '.' and returns the integer part only. That single trap already cost this
// fork every usable .x3g until GPXConvert.cpp pinned the numeric locale; a
// second locale-sensitive parser in the same pipeline is not worth the risk.
// Fixed point sidesteps it entirely, is exact, and leaves an unchanged
// coordinate byte-identical instead of re-rounding it.
constexpr long long SCALE = 1000000LL;

// Reads a decimal number starting at s[i]. On success i points past the
// number; on failure i is untouched.
bool parse_fixed(const std::string& s, size_t& i, long long& out)
{
    const size_t start = i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = (s[i] == '-');
        ++i;
    }
    long long int_part = 0;
    size_t    digits   = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (digits > 9) { i = start; return false; }  // implausible, keep the line verbatim
        int_part = int_part * 10 + (s[i] - '0');
        ++i;
        ++digits;
    }
    long long frac = 0;
    long long div  = 1;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            if (div < SCALE) {              // ignore digits below 1 nm
                frac = frac * 10 + (s[i] - '0');
                div *= 10;
            }
            ++i;
            ++digits;
        }
    }
    if (digits == 0) { i = start; return false; }
    const long long v = int_part * SCALE + frac * (SCALE / div);
    out = neg ? -v : v;
    return true;
}

// Rounds a millimetre value to the fixed-point grid. Plain arithmetic, so it
// stays locale-independent like the rest of this file.
long long round_fixed(double mm)
{
    const double v = mm * static_cast<double>(SCALE);
    return static_cast<long long>(v < 0.0 ? v - 0.5 : v + 0.5);
}

std::string format_fixed(long long v)
{
    const bool         neg = v < 0;
    unsigned long long a   = neg ? static_cast<unsigned long long>(-v)
                                 : static_cast<unsigned long long>(v);
    unsigned long long ip = a / SCALE;
    unsigned long long fp = a % SCALE;

    char frac[6];
    for (int k = 5; k >= 0; --k) {
        frac[k] = static_cast<char>('0' + fp % 10);
        fp /= 10;
    }
    int len = 6;
    while (len > 0 && frac[len - 1] == '0')
        --len;

    std::string out;
    if (neg && (ip != 0 || len > 0))
        out += '-';
    out += std::to_string(ip);
    if (len > 0) {
        out += '.';
        out.append(frac, static_cast<size_t>(len));
    }
    return out;
}

// Positive list of the commands whose X/Y are a position on the bed.
//
// An exclusion list would be wrong here, and dangerously so: G130 sets the
// stepper potentiometers and reads "G130 X127 Y127 A127 B127" - shifted, that
// becomes a wrong motor current. M907 does the same on the Sketch. G10 carries
// a tool offset, G161/G162 take bare axis letters for homing. None of them is
// a coordinate. Only these five are:
//
//   G0 G1   linear move
//   G2 G3   arc move - X/Y are the end point; I/J/R are relative and are left
//           alone by construction, since only the letters X and Y are touched
//   G92     define current position; its argument is absolute even inside G91,
//           which is why it is translated regardless of the mode
bool is_motion_command(long long g, bool& is_g92)
{
    is_g92 = (g == 92);
    return g <= 3 || is_g92;
}

std::string translate_line(const std::string& line, long long sx, long long sy, bool& absolute)
{
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    if (i >= line.size() || (line[i] != 'G' && line[i] != 'g'))
        return line;

    size_t    d      = i + 1;
    long long g      = 0;
    size_t    digits = 0;
    while (d < line.size() && line[d] >= '0' && line[d] <= '9') {
        if (digits > 4) return line;
        g = g * 10 + (line[d] - '0');
        ++d;
        ++digits;
    }
    if (digits == 0)
        return line;

    if (g == 90) { absolute = true;  return line; }
    if (g == 91) { absolute = false; return line; }

    bool is_g92 = false;
    if (!is_motion_command(g, is_g92))
        return line;
    // In relative mode X/Y are distances, not positions - shifting them would
    // corrupt the move. G92 is always absolute.
    if (!absolute && !is_g92)
        return line;

    std::string out;
    out.reserve(line.size() + 8);
    out.append(line, 0, d);

    size_t p       = d;
    bool   changed = false;
    while (p < line.size()) {
        const char c = line[p];
        if (c == ';') {                       // rest of line is a comment
            out.append(line, p, std::string::npos);
            p = line.size();
            break;
        }
        if (c == '(') {                       // parenthetical comment
            const size_t e = line.find(')', p);
            if (e == std::string::npos) {
                out.append(line, p, std::string::npos);
                p = line.size();
                break;
            }
            out.append(line, p, e - p + 1);
            p = e + 1;
            continue;
        }
        if (c == 'X' || c == 'x' || c == 'Y' || c == 'y') {
            size_t    q = p + 1;
            long long v = 0;
            if (parse_fixed(line, q, v)) {
                const bool is_x = (c == 'X' || c == 'x');
                out += c;
                out += format_fixed(v + (is_x ? sx : sy));
                changed = true;
                p       = q;
                continue;
            }
        }
        out += c;
        ++p;
    }
    return changed ? out : line;
}

// "; MB_NATIVE G92 X152 Y72 Z0 A0 B0" -> "G92 X152 Y72 Z0 A0 B0", verbatim.
// See MakerBotCoords.hpp for why those lines are hidden from Orca.
bool expand_native(const std::string& line, std::string& out)
{
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    if (i >= line.size() || line[i] != ';')
        return false;
    ++i;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;

    const size_t tag_len = std::char_traits<char>::length(NATIVE_PREFIX);
    if (i + tag_len > line.size() || line.compare(i, tag_len, NATIVE_PREFIX) != 0)
        return false;
    i += tag_len;
    if (i < line.size() && line[i] != ' ' && line[i] != '\t')
        return false;   // e.g. "MB_NATIVELY" - not our marker
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;

    out = line.substr(i);
    return !out.empty();
}

} // namespace

const char* const NATIVE_PREFIX = "MB_NATIVE";

BedCentre bed_centre(const ConfigBase& config)
{
    BedCentre centre;
    const ConfigOption* opt = config.option("printable_area");
    if (opt == nullptr)
        return centre;
    const auto* pts = dynamic_cast<const ConfigOptionPoints*>(opt);
    if (pts == nullptr || pts->values.size() < 3)
        return centre;

    double xmin = pts->values.front().x(), xmax = xmin;
    double ymin = pts->values.front().y(), ymax = ymin;
    for (const Vec2d& p : pts->values) {
        xmin = std::min(xmin, p.x());
        xmax = std::max(xmax, p.x());
        ymin = std::min(ymin, p.y());
        ymax = std::max(ymax, p.y());
    }
    if (xmax - xmin <= 1.0 || ymax - ymin <= 1.0)
        return centre;

    centre.x     = (xmin + xmax) / 2.0;
    centre.y     = (ymin + ymax) / 2.0;
    centre.valid = true;
    return centre;
}

std::string to_machine_coordinates(const std::string& gcode,
                                   const BedCentre&   centre,
                                   size_t*            native_lines)
{
    if (native_lines)
        *native_lines = 0;
    if (!centre.valid)
        return gcode;

    const long long sx = round_fixed(-centre.x);
    const long long sy = round_fixed(-centre.y);

    std::string out;
    out.reserve(gcode.size() + gcode.size() / 32);

    bool   absolute = true;   // G-code default, and what Orca emits
    size_t pos      = 0;
    while (pos <= gcode.size()) {
        const size_t      nl   = gcode.find('\n', pos);
        const std::string line = (nl == std::string::npos) ? gcode.substr(pos)
                                                           : gcode.substr(pos, nl - pos);
        std::string native;
        if (expand_native(line, native)) {
            if (native_lines)
                ++(*native_lines);
            out += native;
        } else {
            out += translate_line(line, sx, sy, absolute);
        }
        if (nl == std::string::npos)
            break;
        out += '\n';
        pos = nl + 1;
    }
    return out;
}

bool file_to_machine_coordinates(const std::string& src_path,
                                 const std::string& dst_path,
                                 const BedCentre&   centre,
                                 std::string*       error,
                                 size_t*            native_lines)
{
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };

    if (!centre.valid)
        return fail("no usable printable_area - refusing to write machine coordinates blindly");

    std::string gcode;
    {
        std::ifstream in(src_path, std::ios::binary);
        if (!in.is_open())
            return fail("cannot read " + src_path);
        gcode.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (in.bad())
            return fail("error reading " + src_path);
    }

    const std::string shifted = to_machine_coordinates(gcode, centre, native_lines);

    std::ofstream out(dst_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return fail("cannot create " + dst_path);
    out.write(shifted.data(), static_cast<std::streamsize>(shifted.size()));
    out.flush();
    // A truncated intermediate file would reach the printer as a print that
    // stops halfway - surface a full disk here, not there.
    if (!out.good())
        return fail("error writing " + dst_path);
    return true;
}

} // namespace MakerBotCoords
} // namespace Slic3r
