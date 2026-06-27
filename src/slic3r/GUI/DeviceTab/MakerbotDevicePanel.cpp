#include "MakerbotDevicePanel.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/DeviceCore/DevFirmware.h"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/MakerbotLink.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/Jobs/PlaterWorker.hpp"
#include "slic3r/GUI/Jobs/BoostThreadWorker.hpp"

#include <wx/msgdlg.h>
#include <wx/graphics.h>
#include <wx/dcbuffer.h>
#include <wx/choicdlg.h>
#include <wx/log.h>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <fstream>
#include <cstdlib>

namespace Slic3r {
namespace GUI {

namespace {

// Anzahl konfigurierter Extruder (Düsendurchmesser-Array-Länge) - dient zur
// Single-/Dual-Extruder-Unterscheidung innerhalb derselben Baureihe, z.B. um
// bei Legacy-Druckern zwischen "Replicator Single" und "Replicator Dual" /
// "Replicator 2X" zu unterscheiden, ohne dass dafür eine eigene Kategorie
// nötig wäre.
int extruder_count(const DynamicPrintConfig& config)
{
    if (const auto* opt = config.option<ConfigOptionFloats>("nozzle_diameter"))
        return std::max<int>(1, static_cast<int>(opt->values.size()));
    return 1;
}

// YUYV(YUV422) -> wxImage (RGB), anschliessend 90 Grad nach links gedreht.
static wxImage yuyv_to_wximage_rot90ccw(const std::string& yuyv, int w, int h)
{
    auto clamp=[](int x){ return x<0?0:(x>255?255:x); };
    wxImage img(w, h);
    unsigned char* rgb = img.GetData();
    const unsigned char* d = reinterpret_cast<const unsigned char*>(yuyv.data());
    const size_t need = (size_t)w*h*2;
    if (yuyv.size() < need) return wxImage(); // ungueltig
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
    // 90 Grad nach links (gegen Uhrzeigersinn): Sensor ist gedreht verbaut.
    return img.Rotate90(false);
}

// Fortschritts-Ring (Doughnut): grauer Hintergrundring + farbiger Bogen ab
// 12 Uhr im Uhrzeigersinn + Prozenttext mittig. Wert -1 = kein Druck (leer).
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
        const double start = -M_PI / 2.0; // 12 Uhr

        // Hintergrundring (grau)
        gc->SetPen(wxPen(wxColour(80, 80, 80), thickness));
        wxGraphicsPath bg = gc->CreatePath();
        bg.AddArc(cx, cy, r, start, start + 2 * M_PI, true);
        gc->StrokePath(bg);

        // Fortschrittsbogen (Akzentfarbe) - nur wenn progress >= 0
        if (m_progress >= 0 && m_progress <= 100) {
            const double frac = m_progress / 100.0;
            gc->SetPen(wxPen(wxColour(0, 179, 134), thickness)); // #00b386
            wxGraphicsPath fg = gc->CreatePath();
            fg.AddArc(cx, cy, r, start, start + 2 * M_PI * frac, true);
            gc->StrokePath(fg);
        }

        // Prozent-Text mittig
        wxString txt = (m_progress >= 0) ? wxString::Format("%d%%", m_progress)
                                         : wxString::FromUTF8("\xe2\x80\x93"); // Gedankenstrich
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
// KaitenTelemetryJob - Schritt 1 der GUI-Freeze-Behebung.
// process() laeuft auf dem Worker-Thread und fasst NIE ein wx-Widget an -
// nur Daten in Member sammeln. finalize() laeuft auf dem GUI-Thread (Job-
// Vertrag) und wendet die Ergebnisse ueber set_*/apply_*-Methoden an.
// m_active_config wird NUR auf dem GUI-Thread gelesen (beim Erzeugen des
// Jobs in on_telemetry_tick) - process() bekommt einen fertigen PrintHost,
// fasst die Config selbst nie an.
// ---------------------------------------------------------------------------
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
            m_capability_checked_in = false; // neue Sitzung - Capability-Check erneut noetig
        }

        if (!m_capability_checked_in) {
            nlohmann::json cap_resp; std::string cap_err;
            if (m_session->call("has_z_calibration_routine", nlohmann::json::object(), cap_resp, cap_err)) {
                try { m_z_calibration_supported = cap_resp.at("result").get<bool>(); }
                catch (...) { m_z_calibration_supported = true; }
            }
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

            m_status = "Connected";
            if (result.contains("current_process") && result["current_process"].is_object()) {
                const auto& proc = result["current_process"];
                if (proc.contains("step") && proc["step"].is_string())
                    m_status = proc["step"].get<std::string>();
                if (proc.contains("progress") && proc["progress"].is_number())
                    m_progress = proc["progress"].get<int>();
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

                    wxString s = _L("Smart Extruder");
                    if (tool_id >= 0 && tool_id != 99)
                        s += wxString::Format(" (Tool %d)", tool_id);
                    s += ": ";
                    s += fil ? _L("Filament loaded") : _L("no filament");
                    if (preheating)
                        s += wxString::Format(_L(", heating to %d \u00b0C"), tgt);
                    m_extruder_label = s;
                    m_has_extruder_label = true;
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
        eptr = nullptr; // Fehler laufen ueber m_error, nicht ueber Exceptions
        if (canceled || !m_panel) return;

        // Sitzung zurueckschreiben, auch bei Fehler weiter unten - sonst
        // geht eine frisch geoeffnete Sitzung beim naechsten Tick wieder
        // verloren und wir verbinden bei jedem Tick neu.
        m_panel->set_kaiten_session(m_session);
        if (m_capability_checked_out)
            m_panel->apply_capability_check(m_z_calibration_supported);

        if (!m_ok) {
            m_panel->set_telemetry_error(m_error);
            return;
        }

        if (m_has_extruder_label)
            m_panel->set_extruder_label(m_extruder_label);
        m_panel->set_z_offset_controls_enabled(m_status == "Idle");
        m_panel->update_telemetry_ui(m_status, m_temp_ext, m_temp_chamber, m_progress);
    }

private:
    MakerbotDevicePanel*            m_panel;
    std::unique_ptr<PrintHost>      m_host;
    std::shared_ptr<KaitenSession>  m_session;
    bool m_capability_checked_in;
    bool m_capability_checked_out   = false;
    bool m_z_calibration_supported  = true;
    bool m_ok                       = false;
    std::string m_error;
    std::string m_status            = "Connected";
    int  m_temp_ext                 = -1;
    int  m_temp_chamber             = -1;
    int  m_progress                 = -1;
    bool m_has_extruder_label       = false;
    wxString m_extruder_label;
};

// ---------------------------------------------------------------------------
// KaitenCameraJob - Schritt 2 der GUI-Freeze-Behebung.
// Laeuft auf demselben m_kaiten_worker wie KaitenTelemetryJob - dieselbe
// sequentielle Job-Queue schliesst die Session-Oeffnungs-Race zwischen
// Kamera- und Telemetrie-Pfad architektonisch aus (siehe Kommentar oben).
// process() (Worker-Thread) baut nur das wxImage (reine Pixel-Arithmetik,
// kein natives Fenster involviert) - die Zuweisung an das wxStaticBitmap
// passiert erst in finalize() (GUI-Thread) ueber apply_camera_frame().
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

        if (!m_session || !m_session->is_open()) {
            std::string err;
            m_session = mb->open_kaiten_session(err);
            if (!m_session) return; // kein Bild diesen Tick - Telemetrie zeigt den Fehler
        }

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

MakerbotDevicePanel::MakerbotDevicePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY), m_active_config(nullptr)
{
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    // Initialize a default placeholder image for the camera before the stream connects
    m_raw_camera_frame = wxImage(FromDIP(640), FromDIP(480), true);
    m_raw_camera_frame.InitAlpha();

    this->SetSizer(m_main_sizer);

    // Bind the telemetry timer to the tick event handler
    // P5c: explizite IDs fuer beide Timer (s. Kommentar in der .hpp) -
    // sonst wuerde der generische wxEVT_TIMER-Bind (wxID_ANY) auch die
    // Kamera-Tick-Events an on_telemetry_tick routen statt an on_camera_tick.
    m_telemetry_timer.SetOwner(this, ID_TELEMETRY_TIMER);
    this->Bind(wxEVT_TIMER, &MakerbotDevicePanel::on_telemetry_tick, this, ID_TELEMETRY_TIMER);
    m_camera_timer.SetOwner(this, ID_CAMERA_TIMER);
    this->Bind(wxEVT_TIMER, &MakerbotDevicePanel::on_camera_tick, this, ID_CAMERA_TIMER);
}

MakerbotDevicePanel::~MakerbotDevicePanel() {
    stop_telemetry_polling();
}

// -----------------------------------------------------------------------------------------
// Öffnet die persistente Klartext-kaiten-Session (Port 9999) bei Bedarf.
// Nur für Birdwing relevant - Lava/Method und UltiMaker nutzen andere
// Protokolle (HTTP/REST) und sind hier nicht eingebunden.
// -----------------------------------------------------------------------------------------
bool MakerbotDevicePanel::ensure_kaiten_session(std::string& error) {
    if (m_kaiten_session && m_kaiten_session->is_open())
        return true;
    if (!m_active_config) { error = "No active printer configuration."; return false; }

    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(const_cast<DynamicPrintConfig*>(m_active_config)));
    auto* mb = dynamic_cast<MakerbotLink*>(host.get());
    if (!mb) { error = "Active printer is not a MakerbotLink host."; return false; }

    m_kaiten_session = mb->open_kaiten_session(error);
    m_capability_checked = false; // neue Sitzung - Capability-Check erneut nötig
    return m_kaiten_session != nullptr;
}

// -----------------------------------------------------------------------------------------
// Kategorie-Zuordnung: bestimmt, welche der vier Baureihen aktiv ist.
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
// Core UI Builder: baut die UI ausschließlich aus den Sektionen zusammen, die
// für die aktive Kategorie tatsächlich Sinn ergeben.
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::update_ui_for_printer(const DynamicPrintConfig& config) {
    m_active_config = &config;
    m_category = category_for_config(config);

    // Clear existing UI elements to prevent stacking during printer switch
    m_main_sizer->Clear(true);
    m_col_left = nullptr;   // gehoeren dem soeben geleerten Sizer-Baum
    m_col_right = nullptr;
    m_camera_bitmap = nullptr;
    m_zoom_slider = nullptr;
    m_z_offset_slider = nullptr;
    m_z_offset_text = nullptr;
    m_btn_z_calib = m_btn_load_fil = m_btn_unload_fil = m_btn_firmware_update = nullptr;
    m_btn_start_print = nullptr;
    m_progress_donut = nullptr;

    // Eine offene Sitzung gehört zum VORHERIGEN Drucker - sonst würden wir
    // nach einem Druckerwechsel stillschweigend weiter mit dem alten Host
    // sprechen.
    if (m_kaiten_session) {
        m_kaiten_session->close();
        m_kaiten_session.reset();
    }
    m_capability_checked = false;

    const bool is_networked = (m_category != MBDeviceCategory::Legacy);

    // Netzwerk-Druckerfamilien: zweispaltiges Layout.
    //   LINKS  = Kamera (gross), RECHTS = Status + Z-Offset + Steuerung.
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
        // Legacy (Cupcake...Replicator 2X): kein Netzwerk, keine Kamera,
        // kein RPC - nur statische Infos + Firmware-Flash via avrdude.
        build_legacy_static_info_section();
        build_firmware_section();
    }

    // Refresh UI Layout hierarchy to display the updated nodes
    this->Layout();

    // Live-Polling ergibt nur bei Netzwerk-Druckern einen Sinn.
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
    zoom_sizer->Add(zoom_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
    zoom_sizer->Add(m_zoom_slider, 1, wxEXPAND | wxALL, FromDIP(5));
    camera_sizer->Add(zoom_sizer, 0, wxEXPAND | wxALL, FromDIP(2));

    // In die linke Spalte (P5a). Fallback auf m_main_sizer, falls (Legacy o.ae.)
    // kein Spalten-Layout aktiv ist.
    (m_col_left ? m_col_left : m_main_sizer)->Add(camera_sizer, 1, wxEXPAND | wxALL, FromDIP(5));

    m_zoom_slider->Bind(wxEVT_SLIDER, &MakerbotDevicePanel::on_zoom_changed, this);
}

// -----------------------------------------------------------------------------------------
// 2. GLOBAL Z-OFFSET (Birdwing/Lava/UltiMaker)
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_z_offset_section() {
    wxStaticBoxSizer* z_offset_sizer = new wxStaticBoxSizer(wxHORIZONTAL, this, _L("Global Z-Offset Calibration"));

    // Slider values range from -200 to 200, representing -2.00 mm to +2.00 mm
    m_z_offset_slider = new wxSlider(this, wxID_ANY, 0, -200, 200, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
    m_z_offset_text = new wxTextCtrl(this, wxID_ANY, "0.00", wxDefaultPosition, wxSize(FromDIP(60), -1), wxTE_PROCESS_ENTER | wxTE_RIGHT);
    wxStaticText* z_unit = new wxStaticText(this, wxID_ANY, "mm");

    z_offset_sizer->Add(m_z_offset_slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    z_offset_sizer->Add(m_z_offset_text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    z_offset_sizer->Add(z_unit, 0, wxALIGN_CENTER_VERTICAL, FromDIP(5));

    (m_col_right ? m_col_right : m_main_sizer)->Add(z_offset_sizer, 0, wxEXPAND | wxALL, FromDIP(5));

    m_z_offset_text->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& e){
        wxCommandEvent dummy; on_z_offset_slider_changed(dummy);
    });
    m_z_offset_text->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e){
        wxCommandEvent dummy; on_z_offset_slider_changed(dummy); e.Skip();
    });
}

// -----------------------------------------------------------------------------------------
// 3. EXTRUDER-INFO & TELEMETRIE (Birdwing/Lava/UltiMaker - Legacy hat eigene
//    statische Sektion, siehe build_legacy_static_info_section())
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_extruder_and_telemetry_section() {
    m_extruder_info_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Printer Status & Hardware"));

    // Dual-Extrusion (Lava/Method, UltiMaker S-Serie) vs. Single Smart
    // Extruder (Birdwing/Z18). Beide Familien sind reine Netzwerkdrucker mit
    // Smart-Extruder-Generation - "Smart Extruder" ist hier korrekt, im
    // Gegensatz zur Legacy-Baureihe (siehe unten).
    if (m_category == MBDeviceCategory::Lava || m_category == MBDeviceCategory::UltiMaker) {
        m_lbl_extruder_1 = new wxStaticText(this, wxID_ANY, _L("Extruder 1 (Model): Syncing..."));
        m_lbl_extruder_2 = new wxStaticText(this, wxID_ANY, _L("Extruder 2 (Support): Syncing..."));
        m_extruder_info_sizer->Add(m_lbl_extruder_1, 0, wxALL, FromDIP(2));
        m_extruder_info_sizer->Add(m_lbl_extruder_2, 0, wxALL, FromDIP(2));
    } else {
        m_lbl_extruder_1 = new wxStaticText(this, wxID_ANY, _L("Smart Extruder: Syncing..."));
        m_extruder_info_sizer->Add(m_lbl_extruder_1, 0, wxALL, FromDIP(2));
    }

    m_lbl_telemetry_temp = new wxStaticText(this, wxID_ANY, _L("Temperatures (Extruder / Chamber): -- °C / -- °C"));
    m_lbl_telemetry_status = new wxStaticText(this, wxID_ANY, _L("Status: Connecting..."));
    // Fortschritt als Doughnut-Ring (ersetzt die fruehere Progress-Textzeile).
    ProgressDonut* donut = new ProgressDonut(this, wxSize(FromDIP(110), FromDIP(110)));
    m_progress_donut = donut;
    m_lbl_telemetry_progress = nullptr; // wird nicht mehr als Text gezeigt

    m_extruder_info_sizer->Add(m_lbl_telemetry_temp, 0, wxTOP | wxBOTTOM, FromDIP(5));
    m_extruder_info_sizer->Add(m_lbl_telemetry_status, 0, wxBOTTOM, FromDIP(2));
    m_extruder_info_sizer->Add(donut, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, FromDIP(8));

    (m_col_right ? m_col_right : m_main_sizer)->Add(m_extruder_info_sizer, 0, wxEXPAND | wxALL, FromDIP(5));
}

// -----------------------------------------------------------------------------------------
// 4. HARDWARE-STEUERUNG (Birdwing/Lava/UltiMaker)
//    Die Buttons existieren, lösen aber bewusst noch keine echten Kommandos
//    aus: die kaiten-/REST-Kommando-Namen für Z-Offset-Push, Filament
//    Load/Unload sind noch nicht bestätigt (siehe execute_printer_action()).
//    Auf realer Hardware blind geratene RPC-Aufrufe zu senden ist riskanter
//    als nur Telemetrie falsch anzuzeigen, daher hier bewusst zurückhaltend.
// -----------------------------------------------------------------------------------------
void MakerbotDevicePanel::build_hardware_controls_section() {
    wxBoxSizer* controls_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_z_calib = new wxButton(this, wxID_ANY, _L("Run Z-Calibration"));
    m_btn_load_fil = new wxButton(this, wxID_ANY, _L("Load Filament"));
    m_btn_unload_fil = new wxButton(this, wxID_ANY, _L("Unload Filament"));

    controls_sizer->Add(m_btn_z_calib, 1, wxRIGHT, FromDIP(5));
    controls_sizer->Add(m_btn_load_fil, 1, wxRIGHT, FromDIP(5));
    controls_sizer->Add(m_btn_unload_fil, 1, 0);

    (m_col_right ? m_col_right : m_main_sizer)->Add(controls_sizer, 0, wxEXPAND | wxALL, FromDIP(5));

    // "Druck starten" in eigener Zeile, optisch hervorgehoben (voller Breite).
    m_btn_start_print = new wxButton(this, wxID_ANY, _L("Start Print"));
    m_btn_start_print->SetBackgroundColour(wxColour(0, 179, 134)); // #00b386, P5c-Akzent (wie Donut)
    m_btn_start_print->SetForegroundColour(*wxWHITE);
    (m_col_right ? m_col_right : m_main_sizer)->Add(m_btn_start_print, 0, wxEXPAND | wxALL, FromDIP(5));

    m_btn_z_calib->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("z_calibration"); });
    m_btn_load_fil->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("load_filament"); });
    m_btn_unload_fil->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("unload_filament"); });
    m_btn_start_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { execute_printer_action("start_print"); });
}

// -----------------------------------------------------------------------------------------
// 5. FIRMWARE-FLASH via avrdude (NUR Legacy: Cupcake...Replicator 2X - diese
//    Baureihe nutzt AVR/Sailfish-Firmware über USB-Seriell. Birdwing/Lava/
//    UltiMaker aktualisieren ihre Firmware übers Netzwerk, nicht über
//    avrdude - das ist ein separates, noch nicht begonnenes Feature
//    ("WiFi-Setup via USB", siehe HANDOVER.md) und hier bewusst nicht
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
// 6. STATISCHE INFO-SEKTION (NUR Legacy)
//    Cupcake...Replicator 2X sprechen in dieser Architektur ausschließlich
//    über USB/seriell mit Sailfish/MightyBoard-Firmware - es gibt keinen
//    RPC-/REST-Kanal für Live-Status. Statt einer vorgetäuschten Telemetrie
//    zeigen wir hier nur, was aus der aktiven Konfiguration tatsächlich
//    bekannt ist (Modellname, Extruderzahl).
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
        // set_z_adjusted_offset wurde nur für Birdwing/Z18 bestätigt (Capture
        // vom 2026-06). Für Lava/UltiMaker bewusst kein geratener Aufruf.
        BOOST_LOG_TRIVIAL(info) << "MakerbotDevicePanel: Z-Offset control not confirmed for this printer family, not sent.";
        return;
    }
    std::string error;
    if (!ensure_kaiten_session(error)) {
        BOOST_LOG_TRIVIAL(warning) << "MakerbotDevicePanel: Z-Offset not sent, no session: " << error;
        return;
    }
    nlohmann::json resp;
    const nlohmann::json params = {{"offset", offset_mm}};
    if (!m_kaiten_session->call("set_z_adjusted_offset", params, resp, error)) {
        BOOST_LOG_TRIVIAL(warning) << "MakerbotDevicePanel: set_z_adjusted_offset failed: " << error;
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "MakerbotDevicePanel: Z-Offset set to " << offset_mm << "mm";
}

void MakerbotDevicePanel::execute_printer_action(const std::string& action_id) {
    if (m_category != MBDeviceCategory::Birdwing) {
        wxMessageDialog(this,
            _L("This control has only been confirmed for Birdwing (Z18/Replicator+) "
               "printers so far. No command was sent."),
            _L("Not available for this printer"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }

    if (action_id == "unload_filament") {
        // KEIN bestätigter Methodenname (siehe MakerbotLink.hpp/.cpp Kommentar).
        // process_method/"stop_filament" ist ein Kandidat aus dem Capture,
        // aber seine genaue Wirkung ist nicht eindeutig belegt - daher
        // bewusst nicht verdrahtet.
        wxMessageDialog(this,
            _L("Unload Filament has no confirmed command yet - the capture "
               "didn't show this action being triggered. No command was sent. "
               "If you can capture clicking this on the printer's own screen, "
               "send it over and this gets wired up."),
            _L("Not yet implemented"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }

    std::string error;
    if (!ensure_kaiten_session(error)) {
        wxMessageDialog(this, wxString::Format(_L("Could not connect: %s"), error.c_str()),
            _L("Connection Error"), wxOK | wxICON_ERROR).ShowModal();
        return;
    }

    nlohmann::json resp;
    bool ok = false;
    if (action_id == "start_print") {
        // remote_path aus der pending-Datei lesen (von upload() geschrieben).
        std::string host;
        if (const auto* opt = m_active_config->option<ConfigOptionString>("print_host"))
            host = opt->value;
        const char* home = std::getenv("HOME");
        std::string base = home ? std::string(home) : std::string("/tmp");
        std::string safe = host;
        for (char& c : safe) if (c == '/' || c == ':' || c == '\\') c = '_';
        const std::string pending = base + "/.config/OrcaSlicer/makerbot_pending/" + safe + ".txt";

        std::string remote_path;
        { std::ifstream pf(pending); if (pf) std::getline(pf, remote_path); }
        if (remote_path.empty()) {
            wxMessageDialog(this,
                _L("No uploaded file found for this printer yet.\n\n"
                   "Slice a model and use \"Print\" first to upload it, "
                   "then start the print here."),
                _L("Nothing to print"), wxOK | wxICON_INFORMATION).ShowModal();
            return;
        }

        // Kamera-Sichtpruefung durch den Nutzer + Warnhinweis.
        wxMessageDialog confirm(this,
            _L("Please check the live camera image and make sure the build "
               "plate is completely empty.\n\n"
               "WARNING: If any object or residual material is still on the "
               "plate, the print head can be damaged or the print will fail.\n\n"
               "Is the build plate clear?"),
            _L("Check Build Plate"), wxYES_NO | wxICON_WARNING);
        confirm.SetYesNoLabels(_L("Build plate is clear - Start Print"), _L("Cancel"));
        if (confirm.ShowModal() != wxID_YES)
            return;

        nlohmann::json params;
        params["filepath"] = remote_path;
        params["transfer_wait"] = true; // neue Firmware (newPrintFlow)
        ok = m_kaiten_session->call("print", params, resp, error, 30);
        if (ok) {
            wxMessageDialog(this, _L("Print started."),
                _L("Print"), wxOK | wxICON_INFORMATION).ShowModal();
        }
    } else if (action_id == "z_calibration") {
        ok = m_kaiten_session->call("calibrate_z_offset", nlohmann::json::object(), resp, error);
    } else if (action_id == "load_filament") {
        // tool_index 0: einziger bestätigter Fall im Capture (Single-
        // Extruder-Z18). Für Dual-Extrusion (Lava/UltiMaker) ohnehin oben
        // schon ausgeschlossen - kommt erst mit eigener Bestätigung dazu.
        const nlohmann::json params = {{"tool_index", 0}};
        ok = m_kaiten_session->call("load_filament", params, resp, error);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "MakerbotDevicePanel: unknown action_id '" << action_id << "'";
        return;
    }

    if (!ok) {
        wxMessageDialog(this, wxString::Format(_L("Command failed: %s"), error.c_str()),
            _L("Error"), wxOK | wxICON_ERROR).ShowModal();
    }
    BOOST_LOG_TRIVIAL(info) << "MakerbotDevicePanel: action '" << action_id << "' -> " << (ok ? "OK" : "FAILED: " + error);
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
// Telemetry & MJPEG Polling Logic (nur Birdwing/Lava/UltiMaker)
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
        // Worker stoppen lassen, BEVOR wir unsere eigene Referenz auf die
        // Sitzung unten fallen lassen. Kein explizites close() mehr hier:
        // falls ein Job noch laeuft, haelt seine eigene shared_ptr-Kopie die
        // Sitzung am Leben, bis er fertig ist; der KaitenSession-Destruktor
        // ruft close() automatisch auf DEM Thread auf, der die letzte
        // Referenz fallen laesst - nie gleichzeitig mit einem noch
        // laufenden call() auf dem Worker-Thread.
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

void MakerbotDevicePanel::set_telemetry_error(const std::string& error) {
    if (m_lbl_telemetry_status)
        m_lbl_telemetry_status->SetLabel(wxString::Format(_L("Status: %s"), error.c_str()));
}

void MakerbotDevicePanel::set_extruder_label(const wxString& text) {
    if (m_lbl_extruder_1) {
        m_lbl_extruder_1->SetLabel(text);
        m_lbl_extruder_1->Refresh();
    }
}

void MakerbotDevicePanel::set_z_offset_controls_enabled(bool enabled) {
    if (m_z_offset_slider) m_z_offset_slider->Enable(enabled);
    if (m_z_offset_text)   m_z_offset_text->Enable(enabled);
}

void MakerbotDevicePanel::on_telemetry_tick(wxTimerEvent& event) {
    if (!m_active_config || m_category == MBDeviceCategory::Legacy) return;

    if (m_category != MBDeviceCategory::Birdwing) {
        // Lava/Method (HTTP) und UltiMaker (REST): kein bestätigtes Schema -
        // noch kein Capture für diese Familien.
        if (m_lbl_telemetry_status)
            m_lbl_telemetry_status->SetLabel(_L("Status: Live telemetry not yet implemented for this printer family"));
        return;
    }

    // Schritt 1 der GUI-Freeze-Behebung: ab hier kein Netzwerk-Call mehr auf
    // dem GUI-Thread. Nur PrintHost konstruieren (kein Netzwerk, schnell) und
    // einen Job auf den Worker schieben - der macht den eigentlichen Kaiten-
    // Call und liefert ueber finalize() (GUI-Thread) das Ergebnis zurueck.
    if (!m_kaiten_worker)
        m_kaiten_worker = std::make_unique<PlaterWorker<BoostThreadWorker>>(this, nullptr, "kaiten_telemetry_worker");

    if (!m_kaiten_worker->is_idle())
        return; // voriger Tick laeuft noch (Drucker antwortet langsam) - diesen Tick auslassen

    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(const_cast<DynamicPrintConfig*>(m_active_config)));
    if (!dynamic_cast<MakerbotLink*>(host.get()))
        return; // sollte wegen des category-Checks oben nicht vorkommen

    auto job = std::make_shared<KaitenTelemetryJob>(this, std::move(host), m_kaiten_session, m_capability_checked);
    m_kaiten_worker->push(job);
}

void MakerbotDevicePanel::apply_camera_frame(const wxImage& img) {
    if (!img.IsOk()) return;
    m_raw_camera_frame = img;
    // Zoom anwenden (wie on_zoom_changed), sonst 1:1 anzeigen. Zoom-Status
    // wird hier (GUI-Thread, zum Anwendungszeitpunkt) frisch gelesen statt
    // beim Job-Start mitgegeben - vermeidet einen 1s alten Zoom-Stand.
    if (m_zoom_slider && m_zoom_slider->GetValue() > 100) {
        wxCommandEvent dummy;
        on_zoom_changed(dummy);
    } else if (m_camera_bitmap) {
        m_camera_bitmap->SetBitmap(wxBitmap(m_raw_camera_frame));
        m_camera_bitmap->Refresh();
    }
}

void MakerbotDevicePanel::on_camera_tick(wxTimerEvent& event) {
    // P5c: eigener 1s-Tick, entkoppelt von der 2s-Telemetrie.
    // Schritt 2 der GUI-Freeze-Behebung: kein Netzwerk-Call mehr auf dem
    // GUI-Thread - Job auf DENSELBEN Worker wie die Telemetrie schieben,
    // damit beide nie gleichzeitig eine Session oeffnen koennen.
    if (!m_active_config || m_category != MBDeviceCategory::Birdwing) return;
    if (!m_camera_bitmap) return;

    if (!m_kaiten_worker)
        m_kaiten_worker = std::make_unique<PlaterWorker<BoostThreadWorker>>(this, nullptr, "kaiten_telemetry_worker");

    if (!m_kaiten_worker->is_idle())
        return; // Telemetrie- oder vorheriger Kamera-Job laeuft noch - diesen Tick auslassen

    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(const_cast<DynamicPrintConfig*>(m_active_config)));
    if (!dynamic_cast<MakerbotLink*>(host.get()))
        return;

    auto job = std::make_shared<KaitenCameraJob>(this, std::move(host), m_kaiten_session);
    m_kaiten_worker->push(job);
}

void MakerbotDevicePanel::update_telemetry_ui(const std::string& status, int temp_ext, int temp_bed, int progress) {
    if (m_lbl_telemetry_temp) {
        if (temp_ext >= 0 && temp_bed >= 0)
            m_lbl_telemetry_temp->SetLabel(wxString::Format(_L("Temperatures (Extruder / Chamber): %d °C / %d °C"), temp_ext, temp_bed));
        else
            m_lbl_telemetry_temp->SetLabel(_L("Temperatures (Extruder / Chamber): -- °C / -- °C"));
        m_lbl_telemetry_temp->Refresh();
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
}

} // namespace GUI
} // namespace Slic3r
