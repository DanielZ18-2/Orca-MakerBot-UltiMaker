
#ifndef slic3r_MakerbotLink_hpp_
#define slic3r_MakerbotLink_hpp_

// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// Birdwing: raw SSL TCP port 12309 via Boost.Asio (handshake/token bootstrap)
//           PLUS raw plaintext TCP port 9999 (the actual command/telemetry
//           channel - confirmed via packet capture against a real Z18 with
//           MakerBot Desktop 4.10.1, 2026-06. handshake, authenticate,
//           get_system_information, set_z_adjusted_offset, calibrate_z_offset,
//           load_filament etc. all run on 9999, in plaintext, not over the
//           SSL connection.)
// Lava/Method: HTTP port 2222 via libcurl

#include "PrintHost.hpp"
#include "Http.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <wx/string.h>

namespace Slic3r {

// Persistent plaintext kaiten session on port 9999. Opened once (e.g. when
// the Device tab becomes visible for a Birdwing printer) and reused for
// every subsequent telemetry poll / control call, instead of reconnecting
// per call like birdwing_rpc() does for the SSL channel.
class KaitenSession
{
public:
    KaitenSession();
    ~KaitenSession();

    // Connects to host:9999, sends handshake, then authenticate with the
    // given access_token. Returns false with `error` set on any failure.
    bool open(const std::string& host, const std::string& access_token, std::string& error);

    bool call(const std::string& method, const nlohmann::json& params,
              nlohmann::json& out, std::string& error, int timeout_s = 5,
              const std::string* extra_raw = nullptr);

    // Ruft ein Kamera-Einzelbild ab: sendet request_camera_frame, liest die
    // camera_frame-Notification + 16-Byte-Header + YUYV-Pixeldaten.
    // yuyv_out erhaelt die rohen YUYV(YUV422)-Bytes (width*height*2).
    bool fetch_camera_frame(int& width, int& height, std::string& yuyv_out,
                            std::string& error, int timeout_s = 8);

    bool is_open() const;
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class MakerbotLink : public PrintHost
{
public:
    explicit MakerbotLink(DynamicPrintConfig* config);
    ~MakerbotLink() override = default;

    const char* get_name() const override { return "MakerBot"; }

    bool      test(wxString& curl_info)               const override;
    wxString  get_test_ok_msg()                        const override;
    wxString  get_test_failed_msg(wxString& msg)       const override;
    bool      upload(PrintHostUpload upload_data,
                     ProgressFn prg_fn,
                     ErrorFn    err_fn,
                     InfoFn     info_fn)               const override;

    bool                       has_auto_discovery()        const override { return true;  }
    bool                       can_test()                  const override { return true;  }
    PrintHostPostUploadActions get_post_upload_actions()   const override
        { return PrintHostPostUploadActions(); }
    std::string                get_host()                  const override { return m_host; }

    static constexpr int SSL_PORT_BIRDWING    = 12309;
    static constexpr int LAVA_PORT            = 2222;
    // Confirmed via packet capture (see header comment) - NOT documented
    // anywhere officially, but this is what MakerBot Desktop 4.10.1 actually
    // connects to for every kaiten command/telemetry RPC.
    static constexpr int KAITEN_PLAINTEXT_PORT = 9999;

    bool is_birdwing() const { return m_is_birdwing; }

    // Birdwing authorize result
    enum class BirdwingAuthResult { Success, Timeout, ConnectionFailed };

    // Perform handshake + authorize (blocks for up to timeout_s seconds).
    // On Success, error_or_token contains the one_time_token.
    // Called from BirdwingHandshakeDialog background thread.
    BirdwingAuthResult birdwing_authorize(std::string& error_or_token,
                                           int          timeout_s = 120) const;

    // Low-level raw SSL RPC (Birdwing only) – opens new connection each call
    bool birdwing_rpc(const std::string&    method,
                      const nlohmann::json& params,
                      nlohmann::json&       out,
                      std::string&          error,
                      int                   timeout_s = 10) const;

    // Opens a persistent plaintext kaiten session on port 9999 using this
    // printer's stored access_token (from printhost_password). Caller keeps
    // the returned session alive for the duration of repeated polling/control
    // calls (e.g. while the Device tab is visible) and closes it when done.
    // Returns nullptr with `error` set on failure.
    std::shared_ptr<KaitenSession> open_kaiten_session(std::string& error) const;

    // Ruft ein Kamera-Einzelbild (YUYV) ueber eine offene Session ab.
    bool get_camera_frame(KaitenSession& session, int& width, int& height,
                          std::string& yuyv_out, std::string& error) const;

    // KOMPLETTER korrekter Druckstart-Ablauf in EINEM Schritt, exakt wie die
    // offizielle MakerBot Print Software (print_job_helper.js):
    //   1. print  {filepath: <basename>, transfer_wait: true}
    //   2. put    local_path -> "/current_thing/<basename>"
    // Genau in dieser Reihenfolge. Das Device-Tab ruft dies nach der
    // Bauplatten-Bestaetigung auf. local_path ist die fertige .makerbot-Datei.
    bool kaiten_print_and_upload(KaitenSession& session,
                                 const std::string& local_path,
                                 ProgressFn prg_fn, std::string& error) const;

    // Query connected Smart Extruder type via kaiten get_system_information
    // Returns: "mk13", "mk13_impla", "mk13_experimental", "mk12" or ""
    // KNOWN ISSUE (found 2026-06 via packet capture, not yet fixed): real
    // kaiten responses have toolheads as an OBJECT {"chamber":[...],
    // "extruder":[...]}, not a flat array of {"type_name":...} entries as
    // assumed below - this currently always falls through to the "mk13"
    // fallback on real hardware. The real type requires correlating
    // toolheads.extruder[].tool_id against get_machine_config's
    // extruder_profiles.supported_extruders[tool_id] map. Left as-is here;
    // flagged separately rather than fixed as part of the Device-tab work.
    std::string get_toolhead_type(std::string& error) const;

    // Also try Desktop 3.10 HTTPS auth (port 443) as fallback
    BirdwingAuthResult birdwing_authorize_https(std::string& error_or_token,
                                                 int          timeout_s = 120) const;

private:
    std::string m_host;
    std::string m_access_token;   // Alt-Format (nur falls kein Refresh moeglich)
    std::string m_client_id;
    std::string m_client_secret;  // fuer Token-Refresh bei Wiederverbindung
    std::string m_birdwing_code;  // fuer Token-Refresh bei Wiederverbindung
    bool        m_is_birdwing { false };
    int         m_port        { LAVA_PORT };

    // Holt frischen onetime-access_token via HTTPS:443 (Wiederverbindung)
    bool refresh_access_token(std::string& token_out, std::string& error) const;

    // Birdwing-Dateiupload ueber Kaiten (put_init/put_raw/put_term) auf einer
    // bereits offenen + authentifizierten Session. remote_path z.B.
    // "/home/current_thing/<name>.makerbot". Verifiziert gegen echten Z18.
    bool kaiten_upload_file(KaitenSession& session,
                            const std::string& local_path,
                            const std::string& remote_path,
                            ProgressFn prg_fn, std::string& error) const;
    // Startet den Druck der bereits hochgeladenen Datei.
    bool kaiten_print(KaitenSession& session,
                      const std::string& remote_path,
                      bool new_flow, std::string& error) const;


    // HTTP RPC for Lava/Method
    bool lava_rpc(const std::string&    method,
                  const nlohmann::json& params,
                  nlohmann::json&       out,
                  std::string&          error) const;
};

} // namespace Slic3r
#endif // slic3r_MakerbotLink_hpp_
