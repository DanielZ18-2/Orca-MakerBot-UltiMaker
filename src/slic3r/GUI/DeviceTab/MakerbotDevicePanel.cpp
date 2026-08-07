#include "MakerbotDevicePanel.hpp"
#include <cmath>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include <cstdio>
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/DeviceCore/DevFirmware.h"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/MakerbotLink.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/Jobs/PlaterWorker.hpp"
#include "slic3r/GUI/Jobs/BoostThreadWorker.hpp"
#include <map>
#include "libslic3r/PresetBundle.hpp"

#include <wx/msgdlg.h>
#include <wx/textdlg.h>
#include <functional>
#include <wx/graphics.h>
#include <wx/dcbuffer.h>
#include <wx/choicdlg.h>
#include <wx/utils.h>          // wxLaunchDefaultBrowser (Available-Firmware-Button)
#include <wx/log.h>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <fstream>
#include <cstdlib>

namespace Slic3r {
namespace GUI {

namespace {

// Number of configured extruders (nozzle-diameter array length) - used to
// distinguish single-/dual-extruder within the same product line, e.g. to
// tell "Replicator Single" from "Replicator Dual" /
// "Replicator 2X" on legacy printers without needing a dedicated category
// for it.
int extruder_count(const DynamicPrintConfig& config)
{
    if (const auto* opt = config.option<ConfigOptionFloats>("nozzle_diameter"))
        return std::max<int>(1, static_cast<int>(opt->values.size()));
    return 1;
}

// YUYV(YUV422) -> wxImage (RGB), then rotated 90 degrees counter-clockwise.
static wxImage yuyv_to_wximage_rot90ccw(const std::string& yuyv, int w, int h)
{
    auto clamp=[](int x){ return x<0?0:(x>255?255:x); };
    wxImage img(w, h);
    unsigned char* rgb = img.GetData();
    const unsigned char* d = reinterpret_cast<const unsigned char*>(yuyv.data());
    const size_t need = (size_t)w*h*2;
    if (yuyv.size() < need) return wxImage(); // invalid
    for (int i = 0; i < w*h; i += 2) {
        size_t b = (size_t)i*2;
        int Y0=d[b], U=d[b+1], Y1=d[b+2], V=d[b+3];
        for (int j=0;j<2;++j) {
            int Y = (j==0)?Y0:Y1;
            int C=Y-16, D=U-128, E=V-128;
            int R=clamp((298*C+409*E+128)>>8);
            int G=clamp((298*C-100*D-208*E+128)>>8);
            int B=clamp((298*C+516*D+128)>>8);
            int idx=(i+j)*3;
            rgb[idx]=R; rgb[idx+1]=G; rgb[idx+2]=B;
        }
    }
    // 90 degrees left (counter-clockwise): the sensor is mounted rotated.
    return img.Rotate90(false);
}

// Progress ring (doughnut): grey background ring + coloured arc from
// 12 o'clock clockwise + percent text centered. Value -1 = no print (empty).
class ProgressDonut : public wxPanel {
public:
    ProgressDonut(wxWindow* parent, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &ProgressDonut::on_paint, this);
    }
    void set_progress(int p) {
        int np = (p < 0) ? -1 : (p > 100 ? 100 : p);
        if (np != m_progress) { m_progress = np; Refresh(); }
    }
private:
    int m_progress = -1;
    void on_paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        wxGraphicsContext* gc = wxGraphicsContext::Create(dc);
        if (!gc) return;
        const wxSize sz = GetClientSize();
        const double W = sz.GetWidth(), H = sz.GetHeight();
        const double thickness = std::max(6.0, std::min(W, H) * 0.14);
        const double r = (std::min(W, H) - thickness) / 2.0 - 2.0;
        const double cx = W / 2.0, cy = H / 2.0;
        const double start = -M_PI / 2.0; // 12 o'clock

        // Background ring (grey)
        gc->SetPen(wxPen(wxColour(80, 80, 80), thickness));
        wxGraphicsPath bg = gc->CreatePath();
        bg.AddArc(cx, cy, r, start, start + 2 * M_PI, true);
        gc->StrokePath(bg);

        // Progress arc (accent colour) - only when progress >= 0
        if (m_progress >= 0 && m_progress <= 100) {
            const double frac = m_progress / 100.0;
            gc->SetPen(wxPen(wxColour(0, 179, 134), thickness)); // #00b386
            wxGraphicsPath fg = gc->CreatePath();
            fg.AddArc(cx, cy, r, start, start + 2 * M_PI * frac, true);
            gc->StrokePath(fg);
        }

        // Percent text centered
        wxString txt = (m_progress >= 0) ? wxString::Format("%d%%", m_progress)
                                         : wxString::FromUTF8("\xe2\x80\x93"); // en dash
        wxFont font = GetFont();
        font.SetPointSize(std::max(10, (int)(std::min(W, H) * 0.18)));
        font.SetWeight(wxFONTWEIGHT_BOLD);
        gc->SetFont(font, GetForegroundColour().IsOk() ? GetForegroundColour() : *wxWHITE);
        double tw = 0, th = 0;
        gc->GetTextExtent(txt, &tw, &th);
        gc->DrawText(txt, cx - tw / 2.0, cy - th / 2.0);

        delete gc;
    }
};

} // namespace

// -----------------------------------------------------------------------------------------
// Constructor: Initialize the main UI container and placeholder variables
// -----------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// KaitenTelemetryJob - step 1 of the GUI-freeze fix.
// process() runs on the worker thread and NEVER touches a wx widget -
// it only collects data into members. finalize() runs on the GUI thread (job
// contract) and applies the results via set_*/apply_* methods.
// m_active_config is read ONLY on the GUI thread (when creating the
// job in on_telemetry_tick) - process() receives a ready-made PrintHost,
// it never touches the config itself.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Smart-extruder type names, extracted from tool_mappings.py (firmware Z18
// 2.6.3.736) and verified live against the real printer (2026-06-27,
// kaiten_machine_config_probe.py). toolheads.extruder[].tool_id from the
// normal telemetry indexes DIRECTLY into this - no second RPC stage
// needed (get_machine_config on this firmware only returns
// the same telemetry structure again, no dedicated mapping table).
// ---------------------------------------------------------------------------
static const std::map<int, std::string>& smart_extruder_names()
{
    static const std::map<int, std::string> table = {
        {1,   "Smart Extruder 11.0"},
        {2,   "Smart Extruder 11.1"},
        {3,   "Smart Extruder 11.2"},
        {4,   "Smart Extruder 11.3"},
        {5,   "Smart Extruder 12.0"},
        {6,   "Smart Extruder 12.5"},
        {7,   "Smart Extruder 12.1"},
        {8,   "Smart Extruder+"},
        {9,   "Smart Extruder 12.2"},
        {10,  "Smart Extruder 12.2.1"},
        {11,  "Smart Extruder 12.3"},
        {12,  "Smart Extruder 12.4"},
        {13,  "Smart Extruder 12.6"},
        {14,  "Tough Smart Extruder+"},
        {15,  "Smart Extruder+"},
        {16,  "Tough Smart Extruder+"},
        {17,  "Smart Extruder+"},
        {18,  "Tough Smart Extruder+"},
        {19,  "Smart Extruder+"},
        {20,  "Tough Smart Extruder+"},
        {21,  "Smart Extruder+"},
        {22,  "Tough Smart Extruder+"},
        {99,  "Experimental Extruder"}, // see warning at the top of the patch header
        {100, "Shiny Octo Parakeet"},
    };
    return table;
}

// tool_id -> value for the sidebar smart-extruder dropdown (see
// smart_extruder_sidebar_items_for_config() in Plater.cpp). Birdwing knows
// only these three values; anything else (e.g. older mk12 variants) returns
// "" - deliberately NO change instead of a wrong fallback.
static std::string birdwing_smart_extruder_config_value(int tool_id)
{
    switch (tool_id) {
        case 99: return "mk13_experimental";
        case 14: case 16: case 18: case 20: case 22: return "mk13_impla";
        case 8:  case 15: case 17: case 19: case 21:  return "mk13";
        default: return "";
    }
}

// Reads an integer option (e.g. filament temperature) from the full,
// merged config (printer+print+filament) - 0 if not set
// or the key does not exist (no heating instead of guessing).
static int filament_int_option_or_zero(const char* key)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle) return 0;
    const DynamicPrintConfig& cfg = bundle->full_config();
    if (const auto* opt = cfg.option<ConfigOptionInts>(key)) {
        if (!opt->values.empty())
            return opt->values[0];
    }
    return 0;
}

class KaitenTelemetryJob : public Job {
public:
    KaitenTelemetryJob(MakerbotDevicePanel* panel,
                        std::unique_ptr<PrintHost> host,
                        std::shared_ptr<KaitenSession> session,
                        bool capability_already_checked)
        : m_panel(panel), m_host(std::move(host)), m_session(std::move(session)),
          m_capability_checked_in(capability_already_checked)
    {}

    void process(Ctl& /*ctl*/) override {
        auto* mb = dynamic_cast<MakerbotLink*>(m_host.get());
        if (!mb) { m_error = "Active printer is not a MakerbotLink host."; return; }

        if (!m_session || !m_session->is_open()) {
            std::string err;
            m_session = mb->open_kaiten_session(err);
            if (!m_session) { m_error = err; return; }
            m_capability_checked_in = false; // new session - capability check needed again
        }

        if (!m_capability_checked_in) {
            nlohmann::json cap_resp; std::string cap_err;
            if (m_session->call("has_z_calibration_routine", nlohmann::json::object(), cap_resp, cap_err)) {
                try { m_z_calibration_supported = cap_resp.at("result").get<bool>(); }
                catch (...) { m_z_calibration_supported = true; }
            }
            nlohmann::json zr_resp; std::string zr_err;
            if (m_session->call("get_available_z_offset_adjustment",
                                nlohmann::json::object(), zr_resp, zr_err)) {
                try { m_z_offset_max = zr_resp.at("result").get<double>(); }
                catch (...) { /* Default behalten */ }
            }
            nlohmann::json zc_resp; std::string zc_err;
            if (m_session->call("get_z_adjusted_offset",
                                nlohmann::json::object(), zc_resp, zc_err)) {
                try { m_z_offset_value = zc_resp.at("result").get<double>(); }
                catch (...) { }
            }
            m_capability_checked_in = true;  // fix: capability only once per session
            m_capability_checked_out = true;
        }

        nlohmann::json resp; std::string error;
        if (!m_session->call("get_system_information", nlohmann::json::object(), resp, error, 5)) {
            m_error = error; return;
        }

        try {
            const nlohmann::json* infop = nullptr;
            if (resp.contains("params") && resp["params"].is_object()
                && resp["params"].contains("info") && resp["params"]["info"].is_object())
                infop = &resp["params"]["info"];
            else if (resp.contains("result") && resp["result"].is_object())
                infop = &resp["result"];
            else
                throw std::runtime_error("no params.info or result in response");
            const auto& result = *infop;

            // Firmware version: kaiten returns an OBJECT {major,minor,bugfix,build}
            // (server.py::get_server_info). The string "2.6.3.736" exists only in the
            // mDNS record. Handle both forms defensively.
            if (result.contains("firmware_version")) {
                const auto& fv = result["firmware_version"];
                if (fv.is_object()) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                                  fv.value("major", 0), fv.value("minor", 0),
                                  fv.value("bugfix", 0), fv.value("build", 0));
                    m_firmware_version = buf;
                } else if (fv.is_string()) {
                    m_firmware_version = fv.get<std::string>();
                }
            }

            m_status = "Connected";
            if (result.contains("current_process") && result["current_process"].is_object()) {
                const auto& proc = result["current_process"];
                if (proc.contains("step") && proc["step"].is_string())
                    m_status = proc["step"].get<std::string>();
                // 6a: proc["progress"] in heating steps (initial_heating/final_heating) is
                // the HEATING percent (printprocess.py:951,1044-1051), NOT the
                // print progress. m_status already holds the step string here (set
                // above). Fill only in the real print step, otherwise -1 -> donut shows "-".
                if (m_status == "printing" && proc.contains("progress") && proc["progress"].is_number())
                    m_progress = proc["progress"].get<int>();
                else
                    m_progress = -1;
                if (proc.contains("elapsed_time") && proc["elapsed_time"].is_number())
                    m_elapsed_s = proc["elapsed_time"].get<int>();
                int total_s = -1;
                if (proc.contains("time_estimation") && proc["time_estimation"].is_number())
                    total_s = (int)proc["time_estimation"].get<double>();
                else if (proc.contains("duration_s") && proc["duration_s"].is_number())
                    total_s = (int)proc["duration_s"].get<double>();
                if (total_s >= 0 && m_elapsed_s >= 0)
                    m_remaining_s = (total_s > m_elapsed_s) ? (total_s - m_elapsed_s) : 0;
            } else {
                m_status = "Idle";
            }

            if (result.contains("toolheads") && result["toolheads"].is_object()) {
                const auto& th = result["toolheads"];
                if (th.contains("extruder") && th["extruder"].is_array() && !th["extruder"].empty()) {
                    const auto& ex = th["extruder"][0];
                    if (ex.contains("current_temperature"))
                        m_temp_ext = ex["current_temperature"].get<int>();

                    bool fil = ex.value("filament_presence", false);
                    bool preheating = ex.value("preheating", false);
                    int tool_id = ex.value("tool_id", -1);
                    int tgt = ex.value("target_temperature", 0);

                    const auto& names = smart_extruder_names();
                    auto it = names.find(tool_id);
                    if (it != names.end())
                        m_extruder_type_text = it->second; // echter Firmware-Name (tool_mappings.py)
                    else {
                        m_extruder_type_text = _L("Unknown");
                        if (tool_id >= 0)
                            m_extruder_type_text += wxString::Format(" (Tool %d)", tool_id);
                    }

                    wxString st = fil ? _L("Filament loaded") : _L("no filament");
                    if (preheating)
                        st += wxString::Format(_L(", heating to %d \u00b0C"), tgt);
                    m_extruder_status_text = st;
                    m_has_extruder_label = true;
                    m_tool_id = tool_id;
                }
                if (th.contains("chamber") && th["chamber"].is_array() && !th["chamber"].empty()
                    && th["chamber"][0].contains("current_temperature"))
                    m_temp_chamber = th["chamber"][0]["current_temperature"].get<int>();
            }
            m_ok = true;
        } catch (const std::exception& e) {
            m_error = std::string("parse error (") + e.what() + ")";
        }
    }

    void finalize(bool canceled, std::exception_ptr& eptr) override {
        eptr = nullptr; // errors go through m_error, not through exceptions
        if (canceled || !m_panel) return;

        // Write the session back, even on error below - otherwise
        // a freshly opened session is lost on the next tick and we
        // reconnect on every tick.
        m_panel->set_kaiten_session(m_session);
        if (m_capability_checked_out) {
            m_panel->apply_capability_check(m_z_calibration_supported);
            m_panel->apply_z_offset_range(m_z_offset_max);
            m_panel->apply_z_offset_value(m_z_offset_value);
        }

        if (!m_firmware_version.empty())
            m_panel->apply_firmware_version(m_firmware_version);

        if (!m_ok) {
            m_panel->set_telemetry_error(m_error);
            return;
        }

        if (m_has_extruder_label) {
            m_panel->set_extruder_info(m_extruder_type_text, m_extruder_status_text);
            m_panel->m_current_toolhead_id = m_tool_id;
            // Prepare-tab preselection: preselect only, locks nothing. Writes
            // only on an actual change (see
            // Sidebar::set_detected_smart_extruder_type()).
            const std::string se_value = birdwing_smart_extruder_config_value(m_tool_id);
            if (!se_value.empty()) {
                if (Plater* plater = wxGetApp().plater())
                    plater->sidebar().set_detected_smart_extruder_type(0, se_value);
            }
        }
        m_panel->set_z_offset_controls_enabled(m_status == "Idle");
        m_panel->apply_control_button_states(m_status);
        m_panel->update_telemetry_ui(m_status, m_temp_ext, m_temp_chamber, m_progress, m_elapsed_s, m_remaining_s);
    }

private:
    MakerbotDevicePanel*            m_panel;
    std::unique_ptr<PrintHost>      m_host;
    std::shared_ptr<KaitenSession>  m_session;
    bool m_capability_checked_in;
    bool m_capability_checked_out   = false;
    bool m_z_calibration_supported  = true;
    double m_z_offset_max           = 2.0;
    double m_z_offset_value         = 0.0;
    bool m_ok                       = false;
    std::string m_error;
    std::string m_firmware_version;
    std::string m_status            = "Connected";
    int  m_temp_ext                 = -1;
    int  m_temp_chamber             = -1;
    int  m_progress                 = -1;
    int  m_elapsed_s                = -1;
    int  m_remaining_s              = -1;
    bool m_has_extruder_label       = false;
    wxString m_extruder_type_text;
    wxString m_extruder_status_text;
    int  m_tool_id                  = -1;
};

// ---------------------------------------------------------------------------
// KaitenCameraJob - step 2 of the GUI-freeze fix.
// Runs on the same m_kaiten_worker as KaitenTelemetryJob - the same
// sequential job queue closes the session-open race between the
// camera and telemetry path architecturally (see comment above).
// process() (worker thread) only builds the wxImage (pure pixel arithmetic,
// no native window involved) - the assignment to the wxStaticBitmap
// happens only in finalize() (GUI thread) via apply_camera_frame().
// ---------------------------------------------------------------------------
class KaitenCameraJob : public Job {
public:
    KaitenCameraJob(MakerbotDevicePanel* panel,
                     std::unique_ptr<PrintHost> host,
                     std::shared_ptr<KaitenSession> session)
        : m_panel(panel), m_host(std::move(host)), m_session(std::move(session))
    {}

    void process(Ctl& /*ctl*/) override {
        auto* mb = dynamic_cast<MakerbotLink*>(m_host.get());
        if (!mb) return;

        // Breath-1b: the camera opens NO session of its own (passenger). Otherwise
        // a second authenticate -> printer resets -> reconnect storm.
        // If no session is open this frame is skipped; the
        // telemetry opens/holds the single session.
        if (!m_session || !m_session->is_open())
            return;

        int cw = 0, ch = 0; std::string yuyv, cam_err;
        if (mb->get_camera_frame(*m_session, cw, ch, yuyv, cam_err)) {
            m_image = yuyv_to_wximage_rot90ccw(yuyv, cw, ch);
        }
    }

    void finalize(bool canceled, std::exception_ptr& eptr) override {
        eptr = nullptr;
        if (canceled || !m_panel) return;
        m_panel->set_kaiten_session(m_session);
        if (m_image.IsOk())
            m_panel->apply_camera_frame(m_image);
    }

private:
    MakerbotDevicePanel*           m_panel;
    std::unique_ptr<PrintHost>     m_host;
    std::shared_ptr<KaitenSession> m_session;
    wxImage m_image;
};

// ---------------------------------------------------------------------------
// KaitenActionJob - step 3 of the kaiten-session race fix.
// Runs on the SAME m_kaiten_worker as telemetry/camera - closes
// the log-confirmed race (corrupt JSON, torn camera frames)
// architecturally, since all three job types share the same sequential queue.
// process() only makes the RPC call, finalize() (GUI thread) shows the
// success/error dialog - no wx widget access from process().
// ---------------------------------------------------------------------------
class KaitenActionJob : public Job {
public:
    KaitenActionJob(MakerbotDevicePanel* panel,
                     std::unique_ptr<PrintHost> host,
                     std::shared_ptr<KaitenSession> session,
                     std::string method,
                     nlohmann::json params,
                     int timeout_s,
                     wxString success_message,
                     std::function<void(const nlohmann::json&)> on_result = nullptr)
        : m_panel(panel), m_host(std::move(host)), m_session(std::move(session)),
          m_method(std::move(method)), m_params(std::move(params)),
          m_timeout_s(timeout_s), m_success_message(std::move(success_message)),
          m_on_result(std::move(on_result))
    {}

    void process(Ctl& /*ctl*/) override {
        auto* mb = dynamic_cast<MakerbotLink*>(m_host.get());
        if (!mb) { m_error = "Active printer is not a MakerbotLink host."; return; }

        if (!m_session || !m_session->is_open()) {
            std::string err;
            m_session = mb->open_kaiten_session(err);
            if (!m_session) { m_error = err; return; }
        }

        m_ok = m_session->call(m_method, m_params, m_response, m_error, m_timeout_s);
    }

    void finalize(bool canceled, std::exception_ptr& eptr) override {
        eptr = nullptr;
        if (canceled || !m_panel) return;
        m_panel->set_kaiten_session(m_session);

        BOOST_LOG_TRIVIAL(info) << "MakerbotDevicePanel: action '" << m_method
            << "' -> " << (m_ok ? "OK" : "FAILED: " + m_error);

        if (m_ok) {
            if (m_on_result)
                m_on_result(m_response);
            else if (!m_success_message.empty())
                wxMessageDialog(m_panel, m_success_message, _L("Print"), wxOK | wxICON_INFORMATION).ShowModal();
        } else {
            wxMessageDialog(m_panel, wxString::Format(_L("Command failed: %s"), m_error.c_str()),
                _L("Error"), wxOK | wxICON_ERROR).ShowModal();
        }
    }

private:
    MakerbotDevicePanel*           m_panel;
    std::unique_ptr<PrintHost>     m_host;
    std::shared_ptr<KaitenSession> m_session;
    std::string m_method;
    nlohmann::json m_params;
    int m_timeout_s;
    wxString m_success_message;
    std::function<void(const nlohmann::json&)> m_on_result;
    bool m_ok = false;
    std::string m_error;
    nlohmann::json m_response;
};

// Dedicated job for the full print start (print -> put). Unlike
// KaitenActionJob (a single RPC), this one calls the multi-stage
// kaiten_print_and_upload flow with upload progress.
class KaitenPrintJob : public Job {
public:
    KaitenPrintJob(MakerbotDevicePanel* panel,
                   std::unique_ptr<PrintHost> host,
                   std::shared_ptr<KaitenSession> session,
                   std::string local_path)
        : m_panel(panel), m_host(std::move(host)), m_session(std::move(session)),
          m_local_path(std::move(local_path))
    {}

    void process(Ctl& /*ctl*/) override {
        auto* mb = dynamic_cast<MakerbotLink*>(m_host.get());
        if (!mb) { m_error = "Active printer is not a MakerbotLink host."; return; }
        if (!m_session || !m_session->is_open()) {
            std::string err;
            m_session = mb->open_kaiten_session(err);
            if (!m_session) { m_error = err; return; }
        }
        // Upload progress is deliberately NOT mirrored to the GUI here
        // (worker thread). A progress display could be added later via
        // an event; for the first working
        // state the donut/status from telemetry is enough.
        PrintHost::ProgressFn noop = [](Http::Progress, bool&) {};
        m_ok = mb->kaiten_print_and_upload(*m_session, m_local_path, noop, m_error);
    }

    void finalize(bool canceled, std::exception_ptr& eptr) override {
        eptr = nullptr;
        if (canceled || !m_panel) return;
        m_panel->set_kaiten_session(m_session);
        BOOST_LOG_TRIVIAL(info) << "MakerbotDevicePanel: print+upload -> "
            << (m_ok ? "OK" : "FAILED: " + m_error);
        if (m_ok) {
            wxMessageDialog(m_panel, _L("Print started."), _L("Print"),
                wxOK | wxICON_INFORMATION).ShowModal();
        } else {
            wxMessageDialog(m_panel,
                wxString::Format(_L("Could not start print: %s"), m_error.c_str()),
                _L("Error"), wxOK | wxICON_ERROR).ShowModal();
        }
    }

private:
    MakerbotDevicePanel*           m_panel;
    std::unique_ptr<PrintHost>     m_host;
    std::shared_ptr<KaitenSession> m_session;
    std::string                    m_local_path;
    bool                           m_ok = false;
    std::string                    m_error;
};

MakerbotDevicePanel::MakerbotDevicePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY), m_active_config(nullptr)
{
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    // Initialize a default placeholder image for the camera before the stream connects
    m_raw_camera_frame = wxImage(FromDIP(640), FromDIP(480), true);
    m_raw_camera_frame.InitAlpha();

    this->SetSizer(m_main_sizer);

    // Bind the telemetry timer to the tick event handler
    // P5c: explicit IDs for both timers (see comment in the .hpp) -
    // otherwise the generic wxEVT_TIMER bind (wxID_ANY) would also catch the
    // route camera-tick events to on_telemetry_tick instead of on_camera_tick.
    m_telemetry_timer.SetOwner(this, ID_TELEMETRY_TIMER);
    this->Bind(wxEVT_TIMER, &MakerbotDevicePanel::on_telemetry_tick, this, ID_TELEMETRY_TIMER);
    m_camera_timer.SetOwner(this, ID_CAMERA_TIMER);
    this->Bind(wxEVT_TIMER, &MakerbotDevicePanel::on_camera_tick, this, ID_CAMERA_TIMER);
}

MakerbotDevicePanel::~MakerbotDevicePanel() {
    stop_telemetry_polling();
}

// -----------------------------------------------------------------------------------------
// Opens the persistent plaintext kaiten session (port 9999) on demand.
// Relevant only for Birdwing - Lava/Method and UltiMaker use different
// protocols (HTTP/REST) and are not wired in here.
// -----------------------------------------------------------------------------------------
bool MakerbotDevicePanel::ensure_kaiten_session(std::string& error) {
    if (m_kaiten_session && m_kaiten_session->is_open())
        return true;
    if (!m_active_config) { error = "No active printer configuration."; return false; }

    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(const_cast<DynamicPrintConfig*>(m_active_config)));
    auto* mb = dynamic_cast<MakerbotLink*>(host.get());
    if (!mb) { error = "Active printer is not a MakerbotLink host."; return false; }

    m_kaiten_session = mb->open_kaiten_session(error);
    m_capability_checked = false; // new session - capability check needed again
    return m_kaiten_session != nullptr;
}

// -----------------------------------------------------------------------------------------
// Category mapping: determines which of the four product lines is active.
// -----------------------------------------------------------------------------------------
MBDeviceCategory MakerbotDevicePanel::category_for_config(const DynamicPrintConfig& config)
{
    const auto* opt = config.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
    const GCodeFlavor gcf = opt ? opt->value : gcfMakerBotLegacy;

    switch (gcf) {
        case gcfMakerBotBirdwing: return MBDeviceCategory::Birdwing;
        case gcfMakerBotLava:     return MBDeviceCategory::Lava;
        case gcfUltiGCode:        return MBDeviceCategory::UltiMaker;
        case gcfMakerBotLegacy:
        default:                  return MBDeviceCategory::Legacy;
    }
}

// -----------------------------------------------------------------------------------------
// Core UI builder: assembles the UI only from the sections that
// actually make sense for the active category.
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::update_ui_for_printer(const DynamicPrintConfig& config) {
    m_active_config = &config;
    m_category = category_for_config(config);

    // Clear existing UI elements to prevent stacking during printer switch
    m_main_sizer->Clear(true);
    m_col_left = nullptr;   // belong to the just-cleared sizer tree
    m_col_right = nullptr;
    m_camera_bitmap = nullptr;
    m_zoom_slider = nullptr;
    m_z_offset_slider = nullptr;
    m_z_offset_text = nullptr;
    m_btn_pause = m_btn_resume = m_btn_cancel = nullptr;
    m_btn_preheat = nullptr;
    m_btn_rename = nullptr;
    m_btn_z_calib = m_btn_unload_fil = m_btn_firmware_update = nullptr;
    m_btn_start_print = nullptr;
    m_progress_donut = nullptr;

    // An open session belongs to the PREVIOUS printer - otherwise we would
    // silently keep talking to the old host after a printer switch
    // sprechen.
    if (m_kaiten_session) {
        m_kaiten_session->close();
        m_kaiten_session.reset();
    }
    m_capability_checked = false;

    const bool is_networked = (m_category != MBDeviceCategory::Legacy);

    // Network printer families: two-column layout.
    //   LEFT  = camera (large), RIGHT = status + Z-offset + control.
    if (is_networked) {
        wxBoxSizer* columns = new wxBoxSizer(wxHORIZONTAL);
        m_col_left  = new wxBoxSizer(wxVERTICAL);
        m_col_right = new wxBoxSizer(wxVERTICAL);
        // linke Spalte etwas breiter (Kamera), rechte schmaler (Infos/Buttons)
        columns->Add(m_col_left,  3, wxEXPAND | wxRIGHT, FromDIP(5));
        columns->Add(m_col_right, 2, wxEXPAND, 0);
        m_main_sizer->Add(columns, 1, wxEXPAND | wxALL, FromDIP(5));

        build_camera_section();                  // -> m_col_left
        build_extruder_and_telemetry_section();  // -> m_col_right
        build_z_offset_section();                // -> m_col_right
        build_hardware_controls_section();       // -> m_col_right
    } else {
        // Legacy (Cupcake...Replicator 2X): no network, no camera,
        // no RPC - only static info + firmware flash via avrdude.
        build_legacy_static_info_section();
        build_firmware_section();
    }

    // Refresh UI Layout hierarchy to display the updated nodes
    this->Layout();

    // Live polling only makes sense for networked printers.
    if (is_networked)
        start_telemetry_polling();
    else
        stop_telemetry_polling();
}

// -----------------------------------------------------------------------------------------
// 1. WEBCAM & DIGITAL ZOOM (Birdwing/Lava/UltiMaker)
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_camera_section() {
    wxStaticBoxSizer* camera_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Live Camera"));
    m_camera_bitmap = new wxStaticBitmap(this, wxID_ANY, wxBitmap(m_raw_camera_frame));
    camera_sizer->Add(m_camera_bitmap, 1, wxEXPAND | wxALL, FromDIP(5));

    wxBoxSizer* zoom_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* zoom_lbl = new wxStaticText(this, wxID_ANY, _L("Digital Zoom:"));
    m_zoom_slider = new wxSlider(this, wxID_ANY, 100, 100, 300, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
    wxStaticBitmap* zoom_icon = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("settings", this, 16)); // (3)
    zoom_sizer->Add(zoom_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    zoom_sizer->Add(zoom_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
    zoom_sizer->Add(m_zoom_slider, 1, wxEXPAND | wxALL, FromDIP(5));
    camera_sizer->Add(zoom_sizer, 0, wxEXPAND | wxALL, FromDIP(2));

    // Into the left column (P5a). Falls back to m_main_sizer if (legacy etc.)
    // no column layout is active.
    (m_col_left ? m_col_left : m_main_sizer)->Add(camera_sizer, 1, wxEXPAND | wxALL, FromDIP(5));

    m_zoom_slider->Bind(wxEVT_SLIDER, &MakerbotDevicePanel::on_zoom_changed, this);
}

// -----------------------------------------------------------------------------------------
// 2. GLOBAL Z-OFFSET (Birdwing/Lava/UltiMaker)
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_z_offset_section() {
    wxStaticBoxSizer* z_offset_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Global Z-Offset Calibration"));

    // (5a) Icon + slider + value field in one row
    wxBoxSizer* z_row = new wxBoxSizer(wxHORIZONTAL);
    wxStaticBitmap* z_icon = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("param_extruder_clearance", this, 20));
    // Slider values range from -200 to 200, representing -2.00 mm to +2.00 mm
    m_z_offset_slider = new wxSlider(this, wxID_ANY, 0, -200, 200, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
    m_z_offset_text = new wxTextCtrl(this, wxID_ANY, "0.00", wxDefaultPosition, wxSize(FromDIP(60), -1), wxTE_PROCESS_ENTER | wxTE_RIGHT);
    m_z_offset_text->SetToolTip(_L("You can also type the Z-offset value directly here.")); // (5d)
    wxStaticText* z_unit = new wxStaticText(this, wxID_ANY, "mm");
    z_row->Add(z_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    z_row->Add(m_z_offset_slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    z_row->Add(m_z_offset_text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    z_row->Add(z_unit, 0, wxALIGN_CENTER_VERTICAL, FromDIP(5));
    z_offset_sizer->Add(z_row, 0, wxEXPAND | wxALL, FromDIP(2));

    // (5b) Endwert-Labels + Null-Markierung, RESPONSIV: gleiche Spalten-Struktur wie
    // z_row (icon spacer | slider area prop 1 | value+unit spacer). The three
    // Labels split the slider range into thirds -> "0" always sits over the center,
    // unabhaengig von Aufloesung/DPI.
    wxBoxSizer* z_scale = new wxBoxSizer(wxHORIZONTAL);
    z_scale->AddSpacer(FromDIP(20) + FromDIP(6));                 // Icon-Breite + Abstand (wie z_row)
    wxBoxSizer* z_ticks = new wxBoxSizer(wxHORIZONTAL);
    z_ticks->Add(new wxStaticText(this, wxID_ANY, "-2.0"), 1, wxALIGN_LEFT);
    z_ticks->Add(new wxStaticText(this, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL), 1, wxALIGN_CENTRE_HORIZONTAL);
    z_ticks->Add(new wxStaticText(this, wxID_ANY, "+2.0", wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT), 1, wxALIGN_RIGHT);
    z_scale->Add(z_ticks, 1, wxEXPAND | wxRIGHT, FromDIP(10));    // prop 1, aligned with the slider
    z_scale->AddSpacer(FromDIP(60) + FromDIP(2) + FromDIP(24) + FromDIP(5)); // value field + unit + margins
    z_offset_sizer->Add(z_scale, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(2));

    // (5c) Direction hint directly below the slider
    wxStaticText* z_dir = new wxStaticText(this, wxID_ANY,
        _L("Positive = less distance between nozzle and build plate; negative = more distance."));
    z_dir->Wrap(FromDIP(340));
    z_offset_sizer->Add(z_dir, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(2));

    (m_col_right ? m_col_right : m_main_sizer)->Add(z_offset_sizer, 0, wxEXPAND | wxALL, FromDIP(5));

    // Warning (MakerBot convention): a too-positive Z-offset can damage bed/extruder.
    wxStaticText* z_offset_warn = new wxStaticText(this, wxID_ANY,
        _L("Caution: a too-positive Z-offset can damage the build plate and/or the Smart Extruder.\n"
           "The Z-offset can be adjusted live during a print; incorrect values may cause hardware damage."));
    z_offset_warn->SetForegroundColour(wxColour(200, 60, 60));
    z_offset_warn->Wrap(FromDIP(340));
    (m_col_right ? m_col_right : m_main_sizer)->Add(z_offset_warn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));

    // --- Z-Offset-Eingabe (Neuverdrahtung) -------------------------------
    m_z_offset_send_timer = new wxTimer(this);
    this->Bind(wxEVT_TIMER, [this](wxTimerEvent&){
        if (!m_z_offset_slider) return;
        sync_z_offset_to_hardware(m_z_offset_slider->GetValue() / 100.0);
    }, m_z_offset_send_timer->GetId());
    m_z_offset_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&){
        if (!m_z_offset_slider || !m_z_offset_text) return;
        m_z_offset_text->ChangeValue(wxString::Format("%.2f",
            m_z_offset_slider->GetValue() / 100.0));
        if (m_z_offset_send_timer) m_z_offset_send_timer->StartOnce(600);
    });
    m_z_offset_text->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&){
        if (!m_z_offset_slider || !m_z_offset_text) return;
        wxString s = m_z_offset_text->GetValue();
        s.Replace(",", ".");
        double v = 0.0;
        if (!s.ToCDouble(&v)) {
            m_z_offset_text->ChangeValue(wxString::Format("%.2f",
                m_z_offset_slider->GetValue() / 100.0));
            return;
        }
        if (v >  m_z_offset_max_mm) v =  m_z_offset_max_mm;
        if (v < -m_z_offset_max_mm) v = -m_z_offset_max_mm;
        const int ticks = (int)std::lround(v * 100.0);
        m_z_offset_slider->SetValue(ticks);
        m_z_offset_text->ChangeValue(wxString::Format("%.2f", ticks / 100.0));
        if (m_z_offset_send_timer) m_z_offset_send_timer->Stop();
        sync_z_offset_to_hardware(ticks / 100.0);
    });
    m_z_offset_text->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e){
        if (m_z_offset_slider && m_z_offset_text)
            m_z_offset_text->ChangeValue(wxString::Format("%.2f",
                m_z_offset_slider->GetValue() / 100.0));
        e.Skip();
    });
}

// -----------------------------------------------------------------------------------------
// 3. EXTRUDER INFO & TELEMETRY (Birdwing/Lava/UltiMaker - Legacy has its own
//    static section, see build_legacy_static_info_section())
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_extruder_and_telemetry_section() {
    m_extruder_info_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Printer Status & Hardware"));

    // Parameter/value table instead of composed sentences (Daniel,
    // 2026-06-27): fixed parameter label on the left, ONLY the value on the right
    // changes - easier to read than a label whose complete
    // text is recomposed on every update.
    wxFlexGridSizer* grid = new wxFlexGridSizer(2, FromDIP(2), FromDIP(10));
    grid->AddGrowableCol(1);

    auto add_row = [this, grid](const wxString& param_label, wxStaticText** value_out, const wxString& initial_value) {
        wxStaticText* param = new wxStaticText(this, wxID_ANY, param_label);
        wxFont f = param->GetFont(); f.MakeBold(); param->SetFont(f);
        wxStaticText* value = new wxStaticText(this, wxID_ANY, initial_value);
        grid->Add(param, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
        grid->Add(value, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
        *value_out = value;
    };

    // Dual extrusion (Lava/Method, UltiMaker S-line) vs. single smart
    // extruder (Birdwing/Z18). The dual branch deliberately does NOT rely on the table
    // switched - the protocol for it is not confirmed yet.
    if (m_category == MBDeviceCategory::Lava || m_category == MBDeviceCategory::UltiMaker) {
        m_lbl_extruder_1 = new wxStaticText(this, wxID_ANY, _L("Extruder 1 (Model): Syncing..."));
        m_lbl_extruder_2 = new wxStaticText(this, wxID_ANY, _L("Extruder 2 (Support): Syncing..."));
        m_extruder_info_sizer->Add(m_lbl_extruder_1, 0, wxALL, FromDIP(2));
        m_extruder_info_sizer->Add(m_lbl_extruder_2, 0, wxALL, FromDIP(2));
    } else {
        add_row(_L("Smart Extruder Type installed:"), &m_lbl_extruder_type, _L("Syncing..."));
        add_row(_L("Smart Extruder status:"), &m_lbl_extruder_1, _L("Syncing..."));
    }

    add_row(_L("Current Nozzle temperature:"), &m_lbl_telemetry_temp, _L("-- \u00b0C"));
    add_row(_L("Current Printer Chamber temperature:"), &m_lbl_telemetry_temp_chamber, _L("-- \u00b0C"));
    add_row(_L("Current printer operation status:"), &m_lbl_telemetry_status, _L("Connecting..."));

    m_extruder_info_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(2));

    m_lbl_telemetry_progress = nullptr; // no longer shown as text

    wxStaticText* donut_heading = new wxStaticText(this, wxID_ANY, _L("Current progress"));
    wxFont hf = donut_heading->GetFont(); hf.MakeBold(); donut_heading->SetFont(hf);
    m_extruder_info_sizer->Add(donut_heading, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(8));

    // Progress as a doughnut ring (replaces the former progress text line).
    ProgressDonut* donut = new ProgressDonut(this, wxSize(FromDIP(110), FromDIP(110)));
    m_progress_donut = donut;
    m_extruder_info_sizer->Add(donut, 0, wxALIGN_CENTER_HORIZONTAL);
    m_lbl_time_remaining = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    m_extruder_info_sizer->Add(m_lbl_time_remaining, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(8));

    (m_col_right ? m_col_right : m_main_sizer)->Add(m_extruder_info_sizer, 0, wxEXPAND | wxALL, FromDIP(5));
}

// -----------------------------------------------------------------------------------------
// 4. HARDWARE CONTROL (Birdwing/Lava/UltiMaker)
//    The buttons exist but deliberately trigger no real commands
//    yet: the kaiten/REST command names for Z-offset push, filament
//    load/unload are not confirmed yet (see execute_printer_action()).
//    Sending blindly guessed RPC calls to real hardware is riskier
//    than only showing telemetry wrong, hence deliberately conservative here.
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_hardware_controls_section() {
    // Framed "control" card (mockup spec). Order by
    // usage frequency (Daniel, 2026-06-28): print control on top,
    // material below, calibration isolated at the very bottom.
    wxStaticBoxSizer* control_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Steuerung"));

    // Print control: most common actions. RPC confirmed (process_method
    // "suspend"/"resume", cancel_process) - no capability precheck like
    // in the reference; we send directly and show the firmware error
    // if currently unsupported. Untested on real hardware.
    wxBoxSizer* primary_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_pause  = new wxButton(this, wxID_ANY, _L("Pause"));
    m_btn_resume = new wxButton(this, wxID_ANY, _L("Resume"));
    m_btn_cancel = new wxButton(this, wxID_ANY, _L("Cancel"));
    primary_sizer->Add(m_btn_pause, 1, wxRIGHT, FromDIP(5));
    primary_sizer->Add(m_btn_resume, 1, wxRIGHT, FromDIP(5));
    primary_sizer->Add(m_btn_cancel, 1, 0);
    control_box->Add(primary_sizer, 0, wxEXPAND | wxALL, FromDIP(5));

    // Material: preheat + unload filament in one row. "Load"
    // only makes sense at the printer itself (Daniel's decision).
    wxBoxSizer* material_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_preheat = new wxButton(this, wxID_ANY, _L("Preheat"));
    m_btn_unload_fil = new wxButton(this, wxID_ANY, _L("Unload Filament"));
    m_btn_preheat->SetToolTip(_L("Uses nozzle and chamber/bed temperature from "
        "the active filament profile (Filament Settings). Heats the chamber "
        "(Z18) or bed (other Birdwing models) too, so it doesn't need to "
        "heat up later at print start."));
    material_sizer->Add(m_btn_preheat, 1, wxRIGHT, FromDIP(5));
    material_sizer->Add(m_btn_unload_fil, 1, 0);
    control_box->Add(material_sizer, 0, wxEXPAND | wxALL, FromDIP(5));

    // Device: management functions, used less often than material/control,
    // but not as rare as calibration - hence just above it.
    wxStaticText* device_heading = new wxStaticText(this, wxID_ANY, _L("DEVICE"));
    wxFont device_heading_font = device_heading->GetFont();
    device_heading_font.MakeBold();
    device_heading->SetFont(device_heading_font);
    control_box->Add(device_heading, 0, wxLEFT | wxTOP, FromDIP(5));

    wxBoxSizer* device_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_rename = new wxButton(this, wxID_ANY, _L("Rename"));
    m_btn_files = new wxButton(this, wxID_ANY, _L("Files"));
    device_sizer->Add(m_btn_rename, 1, wxRIGHT, FromDIP(5));
    device_sizer->Add(m_btn_files, 1, 0);
    control_box->Add(device_sizer, 0, wxEXPAND | wxALL, FromDIP(5));

    // Calibration: deliberately isolated, full width, at the very bottom - the
    // used the least of everything in this card.
    m_btn_z_calib = new wxButton(this, wxID_ANY, _L("Run Z-Calibration"));
    control_box->Add(m_btn_z_calib, 0, wxEXPAND | wxALL, FromDIP(5));

    (m_col_right ? m_col_right : m_main_sizer)->Add(control_box, 0, wxEXPAND | wxALL, FromDIP(5));

    // "Start print" deliberately stays outside the control card -
    // eigene Zeile, optisch hervorgehoben (voller Breite, Akzentfarbe).
    m_btn_start_print = new wxButton(this, wxID_ANY, _L("Start Print"));
    m_btn_start_print->SetBackgroundColour(wxColour(0, 179, 134)); // #00b386, P5c-Akzent (wie Donut)
    m_btn_start_print->SetForegroundColour(*wxWHITE);
    (m_col_right ? m_col_right : m_main_sizer)->Add(m_btn_start_print, 0, wxEXPAND | wxALL, FromDIP(5));

    // Firmware: link to the firmware collection (placeholder URL, later GitHub
    // source). Deliberately NO flashing from within Orca (liability/upstream) - Birdwing/
    // Lava/UltiMaker update over network or USB stick at the printer. The button
    // needs no member (never toggled dynamically) -> no .hpp change needed.
    {
        wxStaticBoxSizer* fw_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Firmware"));
        wxButton* btn_available_fw = new wxButton(this, wxID_ANY, _L("Available Firmware"));
        btn_available_fw->SetToolTip(_L("Opens the firmware collection in your browser. "
            "Flashing is done via USB stick on the printer, not from OrcaSlicer."));
        fw_box->Add(btn_available_fw, 0, wxEXPAND | wxALL, FromDIP(5));
        (m_col_right ? m_col_right : m_main_sizer)->Add(fw_box, 0, wxEXPAND | wxALL, FromDIP(5));
        btn_available_fw->Bind(wxEVT_BUTTON, [](wxCommandEvent&) {
            // PLACEHOLDER URL: swap later for the GitHub firmware collection.
            wxLaunchDefaultBrowser("https://github.com/DanielZ18-2/Unofficial-OrcaSlicer_for_MakerBot_UltiMaker");
        });
    }

    m_btn_pause->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("pause"); });
    m_btn_resume->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("resume"); });
    m_btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("cancel"); });
    m_btn_rename->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxTextEntryDialog dlg(this, _L("New printer name:"), _L("Rename Printer"), wxEmptyString);
        if (dlg.ShowModal() == wxID_OK) {
            wxString new_name = dlg.GetValue();
            new_name.Trim(true).Trim(false);
            if (!new_name.IsEmpty()) {
                m_pending_rename_name = new_name.ToStdString();
                execute_printer_action("rename");
            }
        }
    });
    m_btn_files->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("files"); });
    m_btn_z_calib->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("z_calibration"); });
    m_btn_preheat->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("preheat"); });
    m_btn_unload_fil->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("unload_filament"); });
    m_btn_start_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("start_print"); });
}

// -----------------------------------------------------------------------------------------
// 5. FIRMWARE FLASH via avrdude (ONLY legacy: Cupcake...Replicator 2X - this
//    line uses AVR/Sailfish firmware over USB-serial. Birdwing/Lava/
//    UltiMaker update their firmware over the network, not via
//    avrdude - that is a separate feature not yet started
//    ("WiFi setup via USB", see HANDOVER.md) and deliberately not
//    nachgebaut.)
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_firmware_section() {
    wxStaticBoxSizer* fw_sizer = new wxStaticBoxSizer(wxHORIZONTAL, this, _L("Maintenance"));
    m_btn_firmware_update = new wxButton(this, wxID_ANY, _L("Flash Firmware (USB/Serial)"));
    fw_sizer->Add(m_btn_firmware_update, 1, wxALL, FromDIP(2));

    m_main_sizer->Add(fw_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    m_btn_firmware_update->Bind(wxEVT_BUTTON, &MakerbotDevicePanel::on_firmware_update_clicked, this);
}

// -----------------------------------------------------------------------------------------
// 6. STATIC INFO SECTION (legacy ONLY)
//    Cupcake...Replicator 2X talk in this architecture exclusively
//    over USB/serial with Sailfish/MightyBoard firmware - there is no
//    RPC/REST channel for live status. Instead of faked telemetry
//    we show here only what is actually known from the active configuration
//    is known (model name, extruder count).
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_legacy_static_info_section() {
    wxStaticBoxSizer* info_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Printer Info"));

    std::string model = "MakerBot Legacy";
    if (m_active_config) {
        if (const auto* opt = m_active_config->option<ConfigOptionString>("printer_model"))
            if (!opt->value.empty())
                model = opt->value;
    }
    const int extruders = m_active_config ? extruder_count(*m_active_config) : 1;

    auto* lbl_model = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Model: %s"), model.c_str()));
    auto* lbl_ext = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Extruders: %d"), extruders));
    auto* lbl_note = new wxStaticText(this, wxID_ANY,
        _L("This printer connects via USB/serial only. Live status, camera and\n"
           "remote control are not available for this generation - use the\n"
           "firmware tool below or your printer's own display panel."));

    info_sizer->Add(lbl_model, 0, wxALL, FromDIP(2));
    info_sizer->Add(lbl_ext, 0, wxALL, FromDIP(2));
    info_sizer->Add(lbl_note, 0, wxALL | wxTOP, FromDIP(6));

    m_main_sizer->Add(info_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM | wxTOP, FromDIP(10));
}

// -----------------------------------------------------------------------------------------
// Digital Zoom Handler: Crops and rescales the raw camera frame dynamically
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::on_zoom_changed(wxCommandEvent& event) {
    if (!m_raw_camera_frame.IsOk() || !m_camera_bitmap || !m_zoom_slider) return;

    int orig_w = m_raw_camera_frame.GetWidth();
    int orig_h = m_raw_camera_frame.GetHeight();
    double zoom_factor = m_zoom_slider->GetValue() / 100.0;

    int crop_w = static_cast<int>(orig_w / zoom_factor);
    int crop_h = static_cast<int>(orig_h / zoom_factor);
    int x_offset = (orig_w - crop_w) / 2;
    int y_offset = (orig_h - crop_h) / 2;

    wxImage zoomed_img = m_raw_camera_frame.GetSubImage(wxRect(x_offset, y_offset, crop_w, crop_h));
    zoomed_img.Rescale(orig_w, orig_h, wxIMAGE_QUALITY_HIGH);

    m_camera_bitmap->SetBitmap(wxBitmap(zoomed_img));
    m_camera_bitmap->Refresh();
}

// -----------------------------------------------------------------------------------------
// Z-Offset Synchronization Handlers
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::on_z_offset_slider_changed(wxCommandEvent& event) {
    if (!m_z_offset_slider || !m_z_offset_text) return;
    double value = m_z_offset_slider->GetValue() / 100.0;

    m_z_offset_text->ChangeValue(wxString::Format("%.2f", value));
    sync_z_offset_to_hardware(value);
}

void MakerbotDevicePanel::sync_z_offset_to_hardware(double offset_mm) {
    if (m_category != MBDeviceCategory::Birdwing) {
        // set_z_adjusted_offset is confirmed only for Birdwing/Z18 (capture
        // from 2026-06). For Lava/UltiMaker deliberately no guessed call.
        BOOST_LOG_TRIVIAL(info) << "MakerbotDevicePanel: Z-Offset control not confirmed for this printer family, not sent.";
        return;
    }
    // Serialize over the SAME worker as telemetry/camera/actions.
    // A direct call on the shared m_kaiten_session from the UI thread caused
    // a data race with the running poll/camera job -> connection reset.
    if (!m_kaiten_worker)
        m_kaiten_worker = std::make_unique<BoostThreadWorker>(nullptr, "kaiten_telemetry_worker");
    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(const_cast<DynamicPrintConfig*>(m_active_config)));
    if (!dynamic_cast<MakerbotLink*>(host.get())) return;
    const nlohmann::json params = {{"offset", offset_mm}};
    auto job = std::make_shared<KaitenActionJob>(this, std::move(host), m_kaiten_session,
                                                  "set_z_adjusted_offset", params, 10,
                                                  std::string(), nullptr);
    m_kaiten_worker->push(job);
    BOOST_LOG_TRIVIAL(info) << "MakerbotDevicePanel: Z-Offset " << offset_mm << "mm ueber Worker eingereiht";
}

// Reads the local .makerbot path from the pending file, shows a
// build-plate confirmation dialog WITH live camera image and, on
// confirmation, starts the correct print->put flow (KaitenPrintJob).
void MakerbotDevicePanel::start_print_with_confirmation() {
    if (m_category != MBDeviceCategory::Birdwing) return;

    // read pending file (now contains the LOCAL path).
    std::string host;
    if (const auto* opt = m_active_config->option<ConfigOptionString>("print_host"))
        host = opt->value;
    const char* home = std::getenv("HOME");
    std::string base = home ? std::string(home) : std::string("/tmp");
    std::string safe = host;
    for (char& c : safe) if (c == '/' || c == ':' || c == '\\') c = '_';
    const std::string pending = base + "/.config/OrcaSlicer/makerbot_pending/" + safe + ".txt";

    std::string local_path;
    { std::ifstream pf(pending); if (pf) std::getline(pf, local_path); }
    if (local_path.empty()) {
        wxMessageDialog(this,
            _L("No sliced file found for this printer yet.\n\n"
               "Slice a model and use \"Print\" first, then start the print here."),
            _L("Nothing to print"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }

    // confirmation dialog WITH live camera image (Daniel's wish). Shows the
    // last received camera frame large, so the user can check the free
    // Bauplatte direkt im Dialog sieht.
    wxDialog dlg(this, wxID_ANY, _L("Check Build Plate"),
                 wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    if (m_raw_camera_frame.IsOk()) {
        // Auf vernuenftige Dialoggroesse skalieren (max. 480 breit).
        wxImage img = m_raw_camera_frame.Copy();
        int w = img.GetWidth(), h = img.GetHeight();
        const int max_w = FromDIP(640); // (2) double size in the clearance dialog
        if (w > max_w && w > 0) {
            int new_h = (int)((double)h * max_w / w);
            img = img.Scale(max_w, new_h, wxIMAGE_QUALITY_HIGH);
        }
        wxStaticBitmap* cam = new wxStaticBitmap(&dlg, wxID_ANY, wxBitmap(img));
        sizer->Add(cam, 0, wxALIGN_CENTER | wxALL, FromDIP(10));
    }

    wxStaticText* warn = new wxStaticText(&dlg, wxID_ANY,
        _L("Make sure the build plate is completely empty.\n\n"
           "WARNING: If any object or residual material is still on the plate, "
           "the print head can be damaged or the print will fail."));
    warn->Wrap(FromDIP(460));
    sizer->Add(warn, 0, wxALL, FromDIP(10));

    wxBoxSizer* btns = new wxBoxSizer(wxHORIZONTAL);
    wxButton* ok = new wxButton(&dlg, wxID_OK, _L("Build plate is clear - Start Print"));
    wxButton* cancel = new wxButton(&dlg, wxID_CANCEL, _L("Cancel"));
    btns->Add(ok, 0, wxRIGHT, FromDIP(5));
    btns->Add(cancel, 0, 0);
    sizer->Add(btns, 0, wxALIGN_CENTER | wxALL, FromDIP(10));

    dlg.SetSizerAndFit(sizer);
    if (dlg.ShowModal() != wxID_OK)
        return;

    // Start the correct print->put flow on the worker.
    if (!m_kaiten_worker)
        m_kaiten_worker = std::make_unique<BoostThreadWorker>(nullptr, "kaiten_telemetry_worker");
    std::unique_ptr<PrintHost> host_obj(PrintHost::get_print_host(const_cast<DynamicPrintConfig*>(m_active_config)));
    if (!dynamic_cast<MakerbotLink*>(host_obj.get())) return;
    auto job = std::make_shared<KaitenPrintJob>(this, std::move(host_obj),
                                                m_kaiten_session, local_path);
    m_kaiten_worker->push(std::move(job));
}

void MakerbotDevicePanel::execute_printer_action(const std::string& action_id) {
    if (m_category != MBDeviceCategory::Birdwing) {
        wxMessageDialog(this,
            _L("This control has only been confirmed for Birdwing (Z18/Replicator+) "
               "printers so far. No command was sent."),
            _L("Not available for this printer"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }

    std::string method;
    nlohmann::json params = nlohmann::json::object();
    int timeout_s = 5;
    wxString success_message; // empty = no success message
    std::function<void(const nlohmann::json&)> on_result; // set = show result instead of a fixed message

    if (action_id == "start_print") {
        // Dedicated multi-stage flow (camera dialog + print->put) - not
        // expressible via the generic KaitenActionJob.
        start_print_with_confirmation();
        return;
    } else if (action_id == "pause") {
        method = "process_method";
        params["method"] = "suspend";
        params["params"] = nlohmann::json::object();
        success_message = _L("Pause command sent to the printer.");
    } else if (action_id == "resume") {
        method = "process_method";
        params["method"] = "resume";
        params["params"] = nlohmann::json::object();
        success_message = _L("Resume command sent to the printer.");
    } else if (action_id == "cancel") {
        // Destructive -> confirm before sending.
        if (wxMessageDialog(this, _L("Cancel the current print? This cannot be undone."),
                _L("Cancel Print"), wxYES_NO | wxICON_WARNING).ShowModal() != wxID_YES)
            return;
        method = "cancel_process";
        success_message = _L("Cancel command sent to the printer.");
    } else if (action_id == "rename") {
        method = "change_machine_name";
        params["machine_name"] = m_pending_rename_name;
    } else if (action_id == "files") {
        // Response format not confirmed - show the raw JSON response
        // instead of guessing a format and parsing it wrong.
        method = "birdwing_list";
        params["path"] = "/";
        on_result = [this](const nlohmann::json& resp) {
            std::string text = resp.contains("result") ? resp.at("result").dump(2) : resp.dump(2);
            wxTextEntryDialog dlg(this,
                _L("Raw response (list format not yet confirmed):"),
                _L("Files"), wxString::FromUTF8(text),
                wxTextEntryDialogStyle | wxTE_MULTILINE);
            dlg.SetSize(FromDIP(wxSize(500, 400)));
            dlg.ShowModal();
        };
    } else if (action_id == "z_calibration") {
        method = "calibrate_z_offset";
    } else if (action_id == "preheat") {
        // temperature_settings: [extruder0, extruder1, chamber/platform, unused].
        // Temperature comes from the active filament profile (Daniel's wish).
        // Index 2 in the kaiten protocol is ONE shared slot for chamber
        // OR bed, depending on hardware (Z18: chamber; other Birdwing models
        // without chamber heating: partly heated bed instead of chamber). First
        // try chamber_temperature (Z18 case), and on 0 fall back to
        // bed_temperature (other model). 0 if nothing is set in the
        // profile (no heating).
        int nozzle_temp = filament_int_option_or_zero("temperature");
        int platform_temp = filament_int_option_or_zero("chamber_temperature");
        if (platform_temp == 0)
            platform_temp = filament_int_option_or_zero("bed_temperature");
        method = "preheat";
        params["temperature_settings"] = {nozzle_temp, 0, platform_temp, 0};
        params["wait_till_heated"] = false;
    } else if (action_id == "unload_filament") {
        // RPC confirmed by source-code analysis (conveyor 3.10.1,
        // birdwing.py:2084-2096) - no longer a guess. tool_index 0:
        // only case for single-extruder Z18 (dual extrusion above
        // already excluded). 215 C PLA default, since no
        // material/temperature selection UI exists.
        method = "unload_filament";
        params["tool_index"] = 0;
        params["temperature_settings"] = 215;
    } else {
        BOOST_LOG_TRIVIAL(warning) << "MakerbotDevicePanel: unknown action_id '" << action_id << "'";
        return;
    }

    // Step 3: over the SAME worker as telemetry/camera - closes
    // the log-confirmed race on m_kaiten_session.
    if (!m_kaiten_worker)
        m_kaiten_worker = std::make_unique<BoostThreadWorker>(nullptr, "kaiten_telemetry_worker");

    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(const_cast<DynamicPrintConfig*>(m_active_config)));
    if (!dynamic_cast<MakerbotLink*>(host.get())) return;

    auto job = std::make_shared<KaitenActionJob>(this, std::move(host), m_kaiten_session,
                                                  method, params, timeout_s, success_message, on_result);
    m_kaiten_worker->push(job);
}

// -----------------------------------------------------------------------------------------
// Firmware Updater (Legacy only, via avrdude)
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::on_firmware_update_clicked(wxCommandEvent& event) {
    if (!m_active_config) {
        wxMessageDialog(this, _L("No active printer configuration found."), _L("Error"), wxOK | wxICON_ERROR).ShowModal();
        return;
    }

    std::string fw_base_dir = resources_dir() + "/firmware/makerbot/";

    wxArrayString choices;
    choices.Add(_L("MakerBot Original Firmware (Latest)"));
    choices.Add(_L("Sailfish Custom Firmware"));

    wxSingleChoiceDialog dialog(this,
        _L("Select the firmware version to flash to the connected printer.\nWarning: Do not disconnect the USB cable during this process."),
        _L("Firmware Selection"), choices);

    if (dialog.ShowModal() == wxID_OK) {
        std::string selected = dialog.GetStringSelection().ToStdString();
        std::string hex_path;

        if (selected.find("Original") != std::string::npos) {
            hex_path = fw_base_dir + "legacy/MightyBoard_RevE_v7.5.hex";
        } else {
            hex_path = fw_base_dir + "sailfish/sailfish_v7.7.hex";
        }

        std::string serial_port = m_active_config->opt_string("serial_port");
        if (serial_port.empty()) {
            wxMessageDialog(this, _L("No serial port configured. Please check your connection settings before flashing."), _L("Connection Error"), wxOK | wxICON_ERROR).ShowModal();
            return;
        }

        std::string flash_log;

        if (DevFirmware::flash_via_usb(hex_path, serial_port, flash_log)) {
            wxMessageDialog(this, _L("Firmware successfully flashed to the printer!"), _L("Success"), wxOK | wxICON_INFORMATION).ShowModal();
        } else {
            wxMessageDialog(this, wxString::Format(_L("Firmware flash failed. Details:\n\n%s"), flash_log), _L("Flash Error"), wxOK | wxICON_ERROR).ShowModal();
        }
    }
}

// -----------------------------------------------------------------------------------------
// Telemetry & MJPEG polling logic (only Birdwing/Lava/UltiMaker)
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::start_telemetry_polling() {
    if (!m_telemetry_timer.IsRunning()) {
        m_telemetry_timer.Start(2000);
        BOOST_LOG_TRIVIAL(info) << "MakerBot/UltiMaker Telemetry polling routine started.";
    }
    if (!m_camera_timer.IsRunning()) {
        m_camera_timer.Start(1000); // P5c: schnellerer, von Telemetrie entkoppelter Tick
        BOOST_LOG_TRIVIAL(info) << "MakerBot/UltiMaker Camera polling routine started.";
    }
}

void MakerbotDevicePanel::stop_telemetry_polling() {
    if (m_telemetry_timer.IsRunning()) {
        m_telemetry_timer.Stop();
        BOOST_LOG_TRIVIAL(info) << "MakerBot/UltiMaker Telemetry polling routine stopped.";
    }
    if (m_camera_timer.IsRunning()) {
        m_camera_timer.Stop();
        BOOST_LOG_TRIVIAL(info) << "MakerBot/UltiMaker Camera polling routine stopped.";
    }
    if (m_kaiten_worker) {
        // Let the worker stop BEFORE we drop our own reference to the
        // session below. No explicit close() here anymore:
        // if a job is still running, its own shared_ptr copy keeps the
        // session alive until it finishes; the KaitenSession destructor
        // calls close() automatically on THE thread that holds the last
        // reference - never at the same time as a still
        // running call() on the worker thread.
        m_kaiten_worker->cancel_all();
        m_kaiten_worker->wait_for_idle(2000);
    }
    m_kaiten_session.reset();
}

void MakerbotDevicePanel::set_kaiten_session(std::shared_ptr<KaitenSession> session) {
    m_kaiten_session = std::move(session);
}

void MakerbotDevicePanel::apply_capability_check(bool supported) {
    m_z_calibration_supported = supported;
    m_capability_checked = true;
    if (m_btn_z_calib)
        m_btn_z_calib->Enable(m_z_calibration_supported);
}

void MakerbotDevicePanel::apply_z_offset_range(double max_mm) {
    // sanity-check the limit (models: 0.4 / 0.8 / 4.0). Fallback 2.0.
    m_z_offset_max_mm = std::max(2.0, (max_mm > 0.0 && max_mm < 50.0) ? max_mm : 2.0);
    const int lim = (int)(m_z_offset_max_mm * 100.0 + 0.5);
    if (m_z_offset_slider) {
        m_z_offset_slider->SetRange(-lim, lim);
        if (m_z_offset_slider->GetValue() >  lim) m_z_offset_slider->SetValue(lim);
        if (m_z_offset_slider->GetValue() < -lim) m_z_offset_slider->SetValue(-lim);
    }
    if (m_z_offset_text)
        m_z_offset_text->ChangeValue(wxString::Format("%.2f",
            m_z_offset_slider ? m_z_offset_slider->GetValue() / 100.0 : 0.0));
}

void MakerbotDevicePanel::apply_z_offset_value(double value_mm) {
    const int ticks = (int)std::lround(value_mm * 100.0);
    if (m_z_offset_slider) {
        int t = ticks;
        const int lim = (int)std::lround(m_z_offset_max_mm * 100.0);
        if (t >  lim) t =  lim;
        if (t < -lim) t = -lim;
        m_z_offset_slider->SetValue(t);
    }
    if (m_z_offset_text)
        m_z_offset_text->ChangeValue(wxString::Format("%.2f", ticks / 100.0));
}

// ---------------------------------------------------------------------------
// Cache the firmware version per DEVICE (print_host) in AppConfig - not in the preset:
// PhysicalPrinter has a key whitelist, and a preset can be shared by
// several devices. Notify once on change. From the
// custom threshold the firmware counts as custom -> material hints drop out.
// ---------------------------------------------------------------------------
static const int MAKERBOT_CUSTOM_FW_MIN_MAJOR = 2;   // <== adjust custom threshold
static const int MAKERBOT_CUSTOM_FW_MIN_MINOR = 7;   // <== (here: from 2.7.x)

static bool makerbot_fw_is_custom(const std::string& v)
{
    int major = 0, minor = 0;
    if (std::sscanf(v.c_str(), "%d.%d", &major, &minor) != 2)
        return false;
    if (major != MAKERBOT_CUSTOM_FW_MIN_MAJOR)
        return major > MAKERBOT_CUSTOM_FW_MIN_MAJOR;
    return minor >= MAKERBOT_CUSTOM_FW_MIN_MINOR;
}

void MakerbotDevicePanel::apply_firmware_version(const std::string& version)
{
    if (version.empty())
        return;
    m_firmware_is_custom = makerbot_fw_is_custom(version);
    if (version == m_firmware_version)
        return;                       // already handled in this session
    m_firmware_version = version;

    std::string host;
    if (m_active_config) {
        if (const auto* opt = m_active_config->option<ConfigOptionString>("print_host"))
            host = opt->value;
    }
    if (host.empty())
        return;

    AppConfig* cfg = wxGetApp().app_config;
    if (!cfg)
        return;
    const std::string previous = cfg->get("makerbot_firmware", host);
    if (!previous.empty() && previous != version) {
        const std::string msg = "MakerBot " + host + ": firmware changed from "
                              + previous + " to " + version + ".";
        if (auto* pl = wxGetApp().plater())
            if (auto* nm = pl->get_notification_manager())
                nm->push_notification(msg);
    }
    if (previous != version)
        cfg->set("makerbot_firmware", host, version);
}

void MakerbotDevicePanel::set_telemetry_error(const std::string& error) {
    if (m_lbl_telemetry_status)
        m_lbl_telemetry_status->SetLabel(wxString::Format(_L("Status: %s"), error.c_str()));
}

void MakerbotDevicePanel::set_extruder_info(const wxString& type_text, const wxString& status_text) {
    if (m_lbl_extruder_type) {
        m_lbl_extruder_type->SetLabel(type_text);
        m_lbl_extruder_type->Refresh();
    }
    if (m_lbl_extruder_1) {
        m_lbl_extruder_1->SetLabel(status_text);
        m_lbl_extruder_1->Refresh();
    }
}

void MakerbotDevicePanel::apply_control_button_states(const std::string& status) {
    // A: enable control buttons per process step (firmware state model).
    // suspend only in step "printing", resume in "suspending"/"suspended";
    // cancel only with an active process; preheat/unload/start only when idle.
    const bool idle      = (status == "Idle");
    const bool printing  = (status == "printing");
    const bool suspended = (status == "suspended" || status == "suspending");
    const bool active    = !idle && (status != "Connected");
    if (m_btn_pause)       m_btn_pause->Enable(printing);
    if (m_btn_resume)      m_btn_resume->Enable(suspended);
    if (m_btn_cancel)      m_btn_cancel->Enable(active);
    if (m_btn_preheat)     m_btn_preheat->Enable(idle);
    if (m_btn_unload_fil)  m_btn_unload_fil->Enable(idle);
    if (m_btn_start_print) m_btn_start_print->Enable(idle);
}

void MakerbotDevicePanel::set_z_offset_controls_enabled(bool enabled) {
    if (m_z_offset_slider) m_z_offset_slider->Enable(enabled);
    if (m_z_offset_text)   m_z_offset_text->Enable(enabled);
}

void MakerbotDevicePanel::on_telemetry_tick(wxTimerEvent& event) {
    if (!m_active_config || m_category == MBDeviceCategory::Legacy) return;

    if (m_category != MBDeviceCategory::Birdwing) {
        // Lava/Method (HTTP) and UltiMaker (REST): no confirmed schema -
        // no capture for these families yet.
        if (m_lbl_telemetry_status)
            m_lbl_telemetry_status->SetLabel(_L("Status: Live telemetry not yet implemented for this printer family"));
        return;
    }

    // Step 1 of the GUI-freeze fix: from here on no network call on
    // the GUI thread. Only construct the PrintHost (no network, fast) and
    // push a job onto the worker - which does the actual kaiten
    // call and returns the result via finalize() (GUI thread).
    // Follow-up fix: plain BoostThreadWorker instead of PlaterWorker<BoostThreadWorker>
    // - PlaterWorker wraps every job in a CursorSetterRAII wrapper
    // (hourglass cursor), right for one-off Plater jobs, but with
    // per-second polling a constantly flickering "loading" cursor.
    // we therefore call process_events() ourselves here instead of
    // automatically via wxEVT_IDLE/PAINT.
    if (!m_kaiten_worker)
        m_kaiten_worker = std::make_unique<BoostThreadWorker>(nullptr, "kaiten_telemetry_worker");
    m_kaiten_worker->process_events();

    if (!m_kaiten_worker->is_idle())
        return; // previous tick still running (printer responds slowly) - skip this tick

    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(const_cast<DynamicPrintConfig*>(m_active_config)));
    if (!dynamic_cast<MakerbotLink*>(host.get()))
        return; // should not happen due to the category check above

    auto job = std::make_shared<KaitenTelemetryJob>(this, std::move(host), m_kaiten_session, m_capability_checked);
    m_kaiten_worker->push(job);
}

void MakerbotDevicePanel::apply_camera_frame(const wxImage& img) {
    if (!img.IsOk()) return;
    // (1)/(2) Scale the live image to double size (one spot -> applies to
    // both live and zoom path, since both use m_raw_camera_frame).
    if (img.GetWidth() > 0 && img.GetHeight() > 0) {
        wxImage scaled = img.Copy();
        scaled.Rescale(img.GetWidth() * 2, img.GetHeight() * 2, wxIMAGE_QUALITY_HIGH);
        m_raw_camera_frame = scaled;
    } else {
        m_raw_camera_frame = img;
    }
    // Apply zoom (like on_zoom_changed), otherwise show 1:1. Zoom state
    // is read fresh here (GUI thread, at apply time) instead of
    // being passed at job start - avoids a 1s-stale zoom value.
    if (m_zoom_slider && m_zoom_slider->GetValue() > 100) {
        wxCommandEvent dummy;
        on_zoom_changed(dummy);
    } else if (m_camera_bitmap) {
        m_camera_bitmap->SetBitmap(wxBitmap(m_raw_camera_frame));
        m_camera_bitmap->Refresh();
    }
}

void MakerbotDevicePanel::on_camera_tick(wxTimerEvent& event) {
    // P5c: own 1s tick, decoupled from the 2s telemetry.
    // Step 2 of the GUI-freeze fix: no more network call on the
    // GUI thread - push a job onto the SAME worker as telemetry,
    // so the two can never open a session at the same time.
    if (!m_active_config || m_category != MBDeviceCategory::Birdwing) return;
    if (!m_camera_bitmap) return;

    if (!m_kaiten_worker)
        m_kaiten_worker = std::make_unique<BoostThreadWorker>(nullptr, "kaiten_telemetry_worker");
    m_kaiten_worker->process_events();

    if (!m_kaiten_worker->is_idle())
        return; // telemetry or previous camera job still running - skip this tick

    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(const_cast<DynamicPrintConfig*>(m_active_config)));
    if (!dynamic_cast<MakerbotLink*>(host.get()))
        return;

    auto job = std::make_shared<KaitenCameraJob>(this, std::move(host), m_kaiten_session);
    m_kaiten_worker->push(job);
}

static wxString mb_fmt_hms(int s) {
    if (s < 0) return wxString();
    int h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (h > 0) return wxString::Format("%d:%02d:%02d", h, m, sec);
    return wxString::Format("%02d:%02d", m, sec);
}

void MakerbotDevicePanel::update_telemetry_ui(const std::string& status, int temp_ext, int temp_bed, int progress, int elapsed_s, int remaining_s) {
    if (m_lbl_telemetry_temp) {
        if (temp_ext >= 0)
            m_lbl_telemetry_temp->SetLabel(wxString::Format(_L("%d \u00b0C"), temp_ext));
        else
            m_lbl_telemetry_temp->SetLabel(_L("-- \u00b0C"));
        m_lbl_telemetry_temp->Refresh();
    }
    if (m_lbl_telemetry_temp_chamber) {
        if (temp_bed >= 0)
            m_lbl_telemetry_temp_chamber->SetLabel(wxString::Format(_L("%d \u00b0C"), temp_bed));
        else
            m_lbl_telemetry_temp_chamber->SetLabel(_L("-- \u00b0C"));
        m_lbl_telemetry_temp_chamber->Refresh();
    }
    if (m_lbl_telemetry_status) {
        m_lbl_telemetry_status->SetLabel(wxString::Format(_L("Status: %s"), status.c_str()));
        m_lbl_telemetry_status->Refresh();
    }
    if (m_progress_donut) {
        static_cast<ProgressDonut*>(m_progress_donut)->set_progress(progress);
    }
    if (m_lbl_telemetry_progress) {
        if (progress >= 0)
            m_lbl_telemetry_progress->SetLabel(wxString::Format(_L("Progress: %d%%"), progress));
        else
            m_lbl_telemetry_progress->SetLabel(_L("Progress: --"));
        m_lbl_telemetry_progress->Refresh();
    }
    if (m_lbl_time_remaining) {
        wxString t;
        if (remaining_s >= 0)      t = wxString::Format(_L("Remaining: %s"), mb_fmt_hms(remaining_s));
        else if (elapsed_s >= 0)   t = wxString::Format(_L("Elapsed: %s"), mb_fmt_hms(elapsed_s));
        m_lbl_time_remaining->SetLabel(t);
        m_lbl_time_remaining->Refresh();
    }
}

} // namespace GUI
} // namespace Slic3r
