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

class Worker;             // Jobs/Worker.hpp - offloads kaiten calls from the GUI thread
class KaitenTelemetryJob; // defined in MakerbotDevicePanel.cpp, needs friend access
class KaitenCameraJob;    // ditto - for the camera tick (step 2)

// Which of the four supported MakerBot/UltiMaker printer families is
// currently active - determines which UI sections make sense at all:
//   Legacy     Cupcake...Replicator 2X: USB/serial only, no network,
//              no camera, no RPC. Needs avrdude firmware flash.
//   Birdwing   Z18 & co: SSL/kaiten RPC, smart extruder, camera.
//   Lava       Method/Sketch: HTTP-RPC, dual extrusion (model/support), camera.
//   UltiMaker  S-line/Cura family: REST-API, usually dual extrusion.
enum class MBDeviceCategory { Legacy, Birdwing, Lava, UltiMaker };

class MakerbotDevicePanel : public wxPanel {
private:
    // --- UI Layout Containers ---
    wxBoxSizer* m_main_sizer;
    wxBoxSizer* m_col_left  = nullptr;  // left column (camera)
    wxBoxSizer* m_col_right = nullptr;  // right column (status/Z-offset/control)
    wxStaticBoxSizer* m_extruder_info_sizer;

    // --- Webcam & digital zoom (Birdwing/Lava/UltiMaker only) ---
    wxStaticBitmap* m_camera_bitmap   = nullptr;
    wxSlider*       m_zoom_slider     = nullptr;
    wxImage         m_raw_camera_frame;

    // --- Global Z-offset calibration (Birdwing/Lava/UltiMaker only) ---
    wxSlider*   m_z_offset_slider = nullptr;
    double      m_z_offset_max_mm = 2.0; // from get_available_z_offset_adjustment
    wxTimer*    m_z_offset_send_timer = nullptr; // debounced send (slider)
    wxTextCtrl* m_z_offset_text   = nullptr;

    // --- Telemetry & extruder information (all families, content varies) ---
    wxStaticText* m_lbl_extruder_1        = nullptr; // value field: "Smart Extruder status"
    wxStaticText* m_lbl_extruder_type     = nullptr; // value field: "Smart Extruder Type installed"
    wxStaticText* m_lbl_extruder_2        = nullptr;
    wxStaticText* m_lbl_telemetry_temp    = nullptr; // value field: "Current Nozzle temperature"
    wxStaticText* m_lbl_telemetry_temp_chamber = nullptr; // value field: "Current Printer Chamber temperature"
    wxStaticText* m_lbl_telemetry_status  = nullptr; // value field: "Current printer operation status" (no prefix anymore)
    wxStaticText* m_lbl_telemetry_progress= nullptr;
    wxStaticText* m_lbl_time_remaining    = nullptr; // remaining time under the donut
    wxPanel*      m_progress_donut       = nullptr; // ProgressDonut* (Cast in .cpp)

    // --- Hardware controls (Birdwing/Lava/UltiMaker only) ---
    wxButton* m_btn_pause       = nullptr;
    wxButton* m_btn_resume      = nullptr;
    wxButton* m_btn_cancel      = nullptr;
    wxButton* m_btn_rename      = nullptr;
    wxButton* m_btn_files       = nullptr;
    std::string m_pending_rename_name; // short-lived scratch buffer for "rename"
    wxButton* m_btn_preheat     = nullptr;
    wxButton* m_btn_unload_fil  = nullptr;
    wxButton* m_btn_start_print = nullptr;

    // --- Firmware flash via avrdude (legacy only: Cupcake...Replicator 2X) ---
    wxButton* m_btn_firmware_update = nullptr;

    // --- Background Tasks & State ---
    // P5c: explicit timer IDs so the wxEVT_TIMER bind routes both timers
    // (telemetry 2s, camera 1s) cleanly to different handlers
    // instead of both to the same one (wxID_ANY would be a wildcard match
    // that would also catch the other timer's events).
    static const int ID_TELEMETRY_TIMER = wxID_HIGHEST + 101;
    static const int ID_CAMERA_TIMER    = wxID_HIGHEST + 102;
    wxTimer m_telemetry_timer;
    wxTimer m_camera_timer; // P5c: own 1s tick just for the camera image
    const DynamicPrintConfig* m_active_config;
    MBDeviceCategory m_category = MBDeviceCategory::Legacy;

    // Persistent plaintext kaiten session (port 9999, Birdwing only - see
    // MakerbotLink.hpp/.cpp). Opened lazily on the first telemetry tick,
    // closed in stop_telemetry_polling()/destructor.
    std::shared_ptr<KaitenSession> m_kaiten_session;
    std::string m_firmware_version;          // last reported firmware version
    bool m_firmware_is_custom = false;       // >= custom threshold -> suppress hints
    bool m_capability_checked = false;      // reset whenever a new session opens

    // tool_id from the last telemetry (toolheads.extruder[0].tool_id),
    // a direct key into the smart-extruder name table. Not yet
    // coupled to the prepare-tab preselection - the widget for it is still
    // missing. -1 = no telemetry received yet.
    int m_current_toolhead_id = -1;

    // Step 1 of the GUI-freeze fix: telemetry kaiten calls now run
    // on their own worker thread instead of synchronously in the timer tick. Declared AFTER
    // m_kaiten_session so the worker is torn down BEFORE the
    // session (reverse declaration order) - no
    // running job may write to it after the session is destroyed.
    // The camera tick (P5c) stays synchronous for now - follows as the next step.
    std::unique_ptr<Worker> m_kaiten_worker;
    friend class KaitenTelemetryJob;
    friend class KaitenCameraJob;
    friend class KaitenActionJob;
    friend class KaitenPrintJob;

    // --- Event Handlers ---
    void on_zoom_changed(wxCommandEvent& event);
    void on_z_offset_slider_changed(wxCommandEvent& event);
    void on_firmware_update_clicked(wxCommandEvent& event);
    void on_telemetry_tick(wxTimerEvent& event);
    void on_camera_tick(wxTimerEvent& event); // P5c: own camera tick (1s)

    // --- Helper Methods ---
    static MBDeviceCategory category_for_config(const DynamicPrintConfig& config);
    bool ensure_kaiten_session(std::string& error); // lazily opens m_kaiten_session
    void sync_z_offset_to_hardware(double offset_mm);
    void execute_printer_action(const std::string& action_id);
    void start_print_with_confirmation(); // print->put after build-plate confirmation
    void update_telemetry_ui(const std::string& status, int temp_ext, int temp_bed, int progress, int elapsed_s, int remaining_s);

    // Write accessors for KaitenTelemetryJob::finalize() (runs on the
    // GUI thread) - the job itself never touches a wx widget directly.
    void set_kaiten_session(std::shared_ptr<KaitenSession> session);
    void apply_capability_check();
    void apply_z_offset_range(double max_mm); // slider limit per model (B1)
    void apply_z_offset_value(double value_mm); // mirror the firmware value into the UI
    void apply_firmware_version(const std::string& version); // cache version + report change
    bool firmware_is_custom() const { return m_firmware_is_custom; }
    void set_telemetry_error(const std::string& error);
    void set_extruder_info(const wxString& type_text, const wxString& status_text);
    void set_z_offset_controls_enabled(bool enabled);
    void apply_control_button_states(const std::string& status); // A: Step-Gating
    void apply_camera_frame(const wxImage& img); // for KaitenCameraJob::finalize()

    // UI builder methods - one per section, each called only when the
    // active category actually supports it.
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