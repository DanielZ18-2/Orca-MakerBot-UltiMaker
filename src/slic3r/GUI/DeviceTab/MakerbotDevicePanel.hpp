#ifndef slic3r_MakerbotDevicePanel_hpp_
#define slic3r_MakerbotDevicePanel_hpp_

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbmp.h>
#include <wx/slider.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/timer.h>
#include <wx/image.h>
#include <memory>

namespace Slic3r {

class DynamicPrintConfig;
class KaitenSession;

namespace GUI {

class Worker;             // Jobs/Worker.hpp - loest Kaiten-Calls vom GUI-Thread
class KaitenTelemetryJob; // in MakerbotDevicePanel.cpp definiert, braucht Friend-Zugriff
class KaitenCameraJob;    // dito - fuer den Kamera-Tick (Schritt 2)

// Welche der vier unterstützten MakerBot/UltiMaker-Druckerfamilien gerade
// aktiv ist - bestimmt, welche UI-Sektionen überhaupt sinnvoll sind:
//   Legacy     Cupcake...Replicator 2X: nur USB/seriell, kein Netzwerk,
//              keine Kamera, kein RPC. Braucht avrdude-Firmware-Flash.
//   Birdwing   Z18 & Co: SSL/kaiten-RPC, Smart Extruder, Kamera.
//   Lava       Method/Sketch: HTTP-RPC, Dual-Extrusion (Model/Support), Kamera.
//   UltiMaker  S-Serie/Cura-Familie: REST-API, i.d.R. Dual-Extrusion.
enum class MBDeviceCategory { Legacy, Birdwing, Lava, UltiMaker };

class MakerbotDevicePanel : public wxPanel {
private:
    // --- UI Layout Containers ---
    wxBoxSizer* m_main_sizer;
    wxBoxSizer* m_col_left  = nullptr;  // linke Spalte (Kamera)
    wxBoxSizer* m_col_right = nullptr;  // rechte Spalte (Status/Z-Offset/Steuerung)
    wxStaticBoxSizer* m_extruder_info_sizer;

    // --- Webcam & Digital Zoom (nur Birdwing/Lava/UltiMaker) ---
    wxStaticBitmap* m_camera_bitmap   = nullptr;
    wxSlider*       m_zoom_slider     = nullptr;
    wxImage         m_raw_camera_frame;

    // --- Global Z-Offset Calibration (nur Birdwing/Lava/UltiMaker) ---
    wxSlider*   m_z_offset_slider = nullptr;
    wxTextCtrl* m_z_offset_text   = nullptr;

    // --- Telemetry & Extruder Information (alle Familien, Inhalt variiert) ---
    wxStaticText* m_lbl_extruder_1        = nullptr; // Wert-Feld: "Smart Extruder status"
    wxStaticText* m_lbl_extruder_type     = nullptr; // Wert-Feld: "Smart Extruder Type installed"
    wxStaticText* m_lbl_extruder_2        = nullptr;
    wxStaticText* m_lbl_telemetry_temp    = nullptr; // Wert-Feld: "Current Nozzle temperature"
    wxStaticText* m_lbl_telemetry_temp_chamber = nullptr; // Wert-Feld: "Current Printer Chamber temperature"
    wxStaticText* m_lbl_telemetry_status  = nullptr; // Wert-Feld: "Current printer operation status" (kein Praefix mehr)
    wxStaticText* m_lbl_telemetry_progress= nullptr;
    wxPanel*      m_progress_donut       = nullptr; // ProgressDonut* (Cast in .cpp)

    // --- Hardware Controls (nur Birdwing/Lava/UltiMaker) ---
    wxButton* m_btn_pause       = nullptr;
    wxButton* m_btn_resume      = nullptr;
    wxButton* m_btn_cancel      = nullptr;
    wxButton* m_btn_rename      = nullptr;
    std::string m_pending_rename_name; // kurzlebiger Zwischenspeicher fuer "rename"
    wxButton* m_btn_z_calib     = nullptr;
    wxButton* m_btn_preheat     = nullptr;
    wxButton* m_btn_unload_fil  = nullptr;
    wxButton* m_btn_start_print = nullptr;

    // --- Firmware-Flash via avrdude (nur Legacy: Cupcake...Replicator 2X) ---
    wxButton* m_btn_firmware_update = nullptr;

    // --- Background Tasks & State ---
    // P5c: explizite Timer-IDs, damit der wxEVT_TIMER-Bind beide Timer
    // (Telemetrie 2s, Kamera 1s) trennscharf an unterschiedliche Handler
    // routet statt beide an denselben (wxID_ANY waere ein Wildcard-Match,
    // der auch Events des jeweils anderen Timers einsammeln wuerde).
    static const int ID_TELEMETRY_TIMER = wxID_HIGHEST + 101;
    static const int ID_CAMERA_TIMER    = wxID_HIGHEST + 102;
    wxTimer m_telemetry_timer;
    wxTimer m_camera_timer; // P5c: eigener 1s-Tick nur fuer das Kamerabild
    const DynamicPrintConfig* m_active_config;
    MBDeviceCategory m_category = MBDeviceCategory::Legacy;

    // Persistent plaintext kaiten session (port 9999, Birdwing only - see
    // MakerbotLink.hpp/.cpp). Opened lazily on the first telemetry tick,
    // closed in stop_telemetry_polling()/destructor.
    std::shared_ptr<KaitenSession> m_kaiten_session;
    bool m_z_calibration_supported = false; // gated via has_z_calibration_routine
    bool m_capability_checked = false;      // reset whenever a new session opens

    // tool_id aus der letzten Telemetrie (toolheads.extruder[0].tool_id),
    // direkter Schluessel in die Smart-Extruder-Namenstabelle. Noch nicht
    // an die Prepare-Tab-Vorauswahl gekoppelt - das Widget dafuer fehlt mir
    // noch. -1 = noch keine Telemetrie erhalten.
    int m_current_toolhead_id = -1;

    // Schritt 1 der GUI-Freeze-Behebung: Telemetrie-Kaiten-Calls laufen jetzt
    // auf einem eigenen Worker-Thread statt synchron im Timer-Tick. NACH
    // m_kaiten_session deklariert, damit der Worker beim Zerstoeren VOR der
    // Session abgebaut wird (umgekehrte Deklarationsreihenfolge) - kein
    // laufender Job darf nach Zerstoerung der Session noch darauf schreiben.
    // Kamera-Tick (P5c) bleibt vorerst synchron - folgt als naechster Schritt.
    std::unique_ptr<Worker> m_kaiten_worker;
    friend class KaitenTelemetryJob;
    friend class KaitenCameraJob;
    friend class KaitenActionJob;

    // --- Event Handlers ---
    void on_zoom_changed(wxCommandEvent& event);
    void on_z_offset_slider_changed(wxCommandEvent& event);
    void on_firmware_update_clicked(wxCommandEvent& event);
    void on_telemetry_tick(wxTimerEvent& event);
    void on_camera_tick(wxTimerEvent& event); // P5c: eigener Kamera-Tick (1s)

    // --- Helper Methods ---
    static MBDeviceCategory category_for_config(const DynamicPrintConfig& config);
    bool ensure_kaiten_session(std::string& error); // lazily opens m_kaiten_session
    void sync_z_offset_to_hardware(double offset_mm);
    void execute_printer_action(const std::string& action_id);
    void update_telemetry_ui(const std::string& status, int temp_ext, int temp_bed, int progress);

    // Schreibzugriffe fuer KaitenTelemetryJob::finalize() (laeuft auf dem
    // GUI-Thread) - der Job selbst fasst nie ein wx-Widget direkt an.
    void set_kaiten_session(std::shared_ptr<KaitenSession> session);
    void apply_capability_check(bool supported);
    void set_telemetry_error(const std::string& error);
    void set_extruder_info(const wxString& type_text, const wxString& status_text);
    void set_z_offset_controls_enabled(bool enabled);
    void apply_camera_frame(const wxImage& img); // fuer KaitenCameraJob::finalize()

    // UI-Bausteinmethoden - eine pro Sektion, jeweils nur aufgerufen wenn die
    // aktive Kategorie sie tatsächlich unterstützt.
    void build_camera_section();
    void build_z_offset_section();
    void build_extruder_and_telemetry_section();
    void build_hardware_controls_section();
    void build_firmware_section();
    void build_legacy_static_info_section();

public:
    MakerbotDevicePanel(wxWindow* parent);
    ~MakerbotDevicePanel() override;

    // Rebuilds the UI dynamically based on the selected printer's category
    void update_ui_for_printer(const DynamicPrintConfig& config);

    // Manage background network requests for live data (no-op for Legacy:
    // Cupcake...Replicator 2X have no network connection in this architecture)
    void start_telemetry_polling();
    void stop_telemetry_polling();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_MakerbotDevicePanel_hpp_