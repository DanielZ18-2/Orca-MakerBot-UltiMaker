// MakerBot / UltiMaker Fork – Orca Slicer 2.4
// MakerbotLink.cpp
//
// Birdwing (Z18, Replicator+, Mini+, 5th Gen):
//   RAW TCP SSL on port 12309 (Boost.Asio) – NOT libcurl HTTP!
//   Protocol: JSON-RPC 2.0, newline-delimited, self-signed cert accepted.
//
// Lava/Method (Method, Method X, Method XL):
//   Plain HTTP JSON-RPC on port 2222 via libcurl (Http class).

#include "MakerbotLink.hpp"
#include <zlib.h>
#include <fstream>
#include <cstdlib>
#include <sys/socket.h> // follow-up fix: SO_RCVTIMEO for KaitenSession reads (ineffective, see below)
#include <sys/time.h>
#include <poll.h> // corrective fix: real timeout via raw poll() before each read_some()
#include "Http.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <nlohmann/json.hpp>
#include <wx/string.h>

#include <random>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>

namespace asio = boost::asio;
namespace ssl  = boost::asio::ssl;
using tcp      = boost::asio::ip::tcp;

namespace Slic3r {

// ── Birdwing Raw SSL RPC Client ───────────────────────────────────────────────
// Sends one JSON-RPC request and reads one JSON-RPC response over raw SSL TCP.
// No HTTP involved – the Z18 speaks newline-terminated JSON directly over TLS.

class BirdwingRpcClient
{
public:
    BirdwingRpcClient(const std::string& host, int port)
        : m_host(host), m_port(port)
        , m_ssl_ctx(ssl::context::tls_client)
        , m_socket(m_io, m_ssl_ctx)
    {
        // Accept self-signed certificates (MakerBot uses vendor-internal CA)
        m_ssl_ctx.set_verify_mode(ssl::verify_none);
    }

    bool connect(std::string& error) {
        try {
            tcp::resolver resolver(m_io);
            auto eps = resolver.resolve(m_host, std::to_string(m_port));
            asio::connect(m_socket.lowest_layer(), eps);
            m_socket.lowest_layer().set_option(tcp::no_delay(true));
            m_socket.handshake(ssl::stream_base::client);
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    // Send one JSON-RPC request (adds \r\n), read one JSON response.
    // Send multiple RPCs on the same connection
    bool call_persistent(const nlohmann::json& request,
                          nlohmann::json&       response,
                          std::string&          error)
    {
        return call(request, response, error, 130); // long timeout for authorize
    }

    bool call(const nlohmann::json& request,
              nlohmann::json&       response,
              std::string&          error,
              int                   timeout_s = 10)
    {
        try {
            const std::string msg = request.dump() + "\r\n";
            asio::write(m_socket, asio::buffer(msg));

            // Set timeout via a deadline on the io_context
            asio::streambuf buf;
            boost::system::error_code ec;
            asio::read_until(m_socket, buf, '\n', ec);
            if (ec && ec != asio::error::eof) {
                error = ec.message();
                return false;
            }

            std::istream is(&buf);
            std::string line;
            std::getline(is, line);
            boost::algorithm::trim(line);
            if (line.empty()) {
                error = "Empty response from MakerBot";
                return false;
            }

            response = nlohmann::json::parse(line);
            if (response.contains("error")) {
                error = response["error"].value("message", "RPC error");
                return false;
            }
            return true;

        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    void close() {
        boost::system::error_code ec;
        m_socket.lowest_layer().close(ec);
    }

private:
    std::string       m_host;
    int               m_port;
    asio::io_context  m_io;
    ssl::context      m_ssl_ctx;
    ssl::stream<tcp::socket> m_socket;
};


// ── KaitenSession: persistent plaintext RPC on port 9999 ─────────────────────
// Confirmed via packet capture (Z18, MakerBot Desktop 4.10.1, 2026-06): the
// real command/telemetry channel is plain TCP on port 9999, newline-
// delimited JSON-RPC 2.0, no TLS at all. handshake -> authenticate(token) ->
// repeated calls, all on the SAME connection - unlike BirdwingRpcClient
// above (SSL/12309), which reconnects per call.

// Corrective fix: SO_RCVTIMEO (see open()) is not honored by Boost.Asio's own
// reactor - read_some() can still block forever.
// poll() is a raw POSIX syscall outside Asio's management and
// therefore reliably provides a timeout. Return: true = data available,
// false = timeout/error (the caller then checks its own, larger
// timeout and retries if needed).
static bool kaiten_wait_readable(int fd, int timeout_ms)
{
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc <= 0) return false; // timeout (0) or error (<0)
    return (pfd.revents & POLLIN) != 0;
}

struct KaitenSession::Impl
{
    std::string      host;
    asio::io_context io;
    tcp::socket      socket{io};
    bool             connected = false;
    int              next_id   = 2; // 0/1 are used up by handshake+authenticate in open()
};

KaitenSession::KaitenSession() : m_impl(std::make_unique<Impl>()) {}
KaitenSession::~KaitenSession() { close(); }

bool KaitenSession::is_open() const { return m_impl && m_impl->connected; }

void KaitenSession::close()
{
    if (!m_impl || !m_impl->connected) return;
    boost::system::error_code ec;
    m_impl->socket.close(ec);
    m_impl->connected = false;
}

bool KaitenSession::call(const std::string& method, const nlohmann::json& params,
                          nlohmann::json& out, std::string& error, int timeout_s,
                          const std::string* extra_raw)
{
    if (!is_open()) { error = "KaitenSession is not connected."; return false; }

    const int req_id = m_impl->next_id++;
    const nlohmann::json request = {
        {"jsonrpc", "2.0"}, {"method", method}, {"params", params}, {"id", req_id}
    };

    try {
        if (extra_raw != nullptr) {
            // put_raw: bare JSON WITHOUT \r\n, then the raw bytes immediately
            // (verifiziert: ein \r\n wuerde als erste 2 Block-Bytes
            // misinterpreted -> wrong CRC / timeout).
            const std::string head = request.dump();
            asio::write(m_impl->socket, asio::buffer(head));
            asio::write(m_impl->socket, asio::buffer(*extra_raw));
        } else {
            const std::string msg = request.dump() + "\r\n";
            asio::write(m_impl->socket, asio::buffer(msg));
        }

        // Kaiten rahmt JSON-Nachrichten per BRACE-COUNTING, NICHT per Newline
        // (verifiziert gegen conveyor/json_reader.py, MakerWare 3.10.1).
        // Also, the printer sends the response as a "system_notification"
        // (params.info), not necessarily as result. We read a complete
        // top-level JSON object (counting braces) until it is closed.
        m_impl->socket.non_blocking(false);
        const auto t_start = std::chrono::steady_clock::now();

        // response, skipping unsolicited system_notification messages
        // (telemetry push of the Z18): skip them, only the message with
        // our req_id - or one with result/error and without a foreign id -
        // counts as the response. Verified against kaiten_upload_probe.py.
        while (true) {
            std::string raw;
            {
                boost::system::error_code ec;
                int depth = 0; bool in_str = false, escaped = false, started = false;
                char c;
                while (true) {
                    if (std::chrono::steady_clock::now() - t_start
                            > std::chrono::seconds(timeout_s)) {
                        error = "Timeout reading from MakerBot (port 9999)";
                        // Breath-1: Session NICHT schliessen. Ein langsamer/
                        // a busy printer should not trigger a token+authenticate
                        // storm that jams the kaiten server.
                        // We catch late responses via req_id matching;
                        // real aborts (RST/EOF) close via the ec branch.
                        return false;
                    }
                    if (!kaiten_wait_readable(m_impl->socket.native_handle(), 200)) continue; // nothing there yet
                    size_t got = m_impl->socket.read_some(asio::buffer(&c, 1), ec);
                    if (ec == asio::error::would_block) continue; // SO_RCVTIMEO expiry, not a real error
                    if (ec) { error = ec.message(); close(); return false; }
                    if (got == 0) continue;
                    if (!started) {
                        if (c == '{' || c == '[') { started = true; depth = 1; raw.push_back(c); }
                        continue; // ignore whitespace/newline before the object
                    }
                    raw.push_back(c);
                    if (in_str) {
                        if (!escaped && c == '"') in_str = false;
                        escaped = (c == '\\') && !escaped;
                        continue;
                    }
                    if (c == '"') in_str = true;
                    else if (c == '{' || c == '[') depth++;
                    else if (c == '}' || c == ']') {
                        depth--;
                        if (depth == 0) break; // complete object read
                    }
                }
            }
            if (raw.empty()) { error = "Empty response from MakerBot (port 9999)"; return false; }

            nlohmann::json msg = nlohmann::json::parse(raw);

            // Does this message belong to our request?
            bool is_our_response = false;
            if (msg.contains("id") && !msg["id"].is_null()) {
                // carries an id -> accept only if it matches our req_id
                try { is_our_response = (msg["id"].get<long long>() == (long long)req_id); }
                catch (...) { is_our_response = false; }
            } else if (msg.contains("result") || msg.contains("error")) {
                // response without id (some firmware) -> treat as our response
                is_our_response = true;
            }
            if (!is_our_response) {
                // unsolicited notification (e.g. system_notification) -> skip
                continue;
            }

            out = std::move(msg);
            if (out.contains("error") && !out["error"].is_null()) {
                error = out["error"].is_object()
                            ? out["error"].value("message", "RPC error")
                            : std::string("RPC error");
                return false;
            }
            return true;
        }
    } catch (const std::exception& e) {
        error = e.what();
        close();
        return false;
    }
}

bool KaitenSession::open(const std::string& host, const std::string& access_token, std::string& error)
{
    close();
    m_impl = std::make_unique<Impl>();
    m_impl->host = host;

    try {
        tcp::resolver resolver(m_impl->io);
        auto eps = resolver.resolve(host, std::to_string(MakerbotLink::KAITEN_PLAINTEXT_PORT));
        asio::connect(m_impl->socket, eps);
        m_impl->socket.set_option(tcp::no_delay(true));
        // Follow-up fix: SO_RCVTIMEO, otherwise a read_some() WITHOUT any
        // response (e.g. printer busy/calibrating) blocks the GUI thread
        // forever - the manual timeout check in call()/
        // fetch_camera_frame() acts only BETWEEN completed reads,
        // never DURING a blocking read_some() without any response.
        // 1s, so the larger timeouts (5s/8s) can still check multiple times
        // can, instead of only once.
        {
            struct timeval tv{};
            tv.tv_sec = 1; tv.tv_usec = 0;
            ::setsockopt(m_impl->socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&tv), sizeof(tv));
        }
        m_impl->connected = true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }

    // NO handshake! Verified against conveyor/machine/birdwing.py:
    // authenticate_connection() calls authenticate directly, without handshake.
    // The handshake belongs only to the initial client-thread setup and
    // is not answered by the printer on the reconnection path
    // (this caused the timeout). The access_token is a ONETIME token that
    // was freshly fetched via HTTPS:443 immediately before (see
    // open_kaiten_session -> refresh_access_token).
    if (access_token.empty()) {
        error = "No fresh access_token - token refresh (HTTPS:443) failed.";
        close();
        return false;
    }

    nlohmann::json resp;
    const nlohmann::json auth_params = {{"access_token", access_token}};
    if (!call("authenticate", auth_params, resp, error, 10)) {
        close();
        return false;
    }

    return true;
}


// ── Konstruktor ───────────────────────────────────────────────────────────────

MakerbotLink::MakerbotLink(DynamicPrintConfig* config)
{
    if (const auto* opt = config->opt<ConfigOptionString>("print_host"))
        m_host = opt->value;

    std::string stored_auth;
    if (const auto* opt = config->opt<ConfigOptionString>("printhost_password"))
        stored_auth = opt->value;

    // Format (NEW, for token refresh on reconnection):
    //   "OrcaSlicer:<client_secret>:<birdwing_code>"
    // Old format (access_token only) is still tolerated: "OrcaSlicer:<token>"
    {
        std::vector<std::string> parts;
        size_t start = 0, pos;
        while ((pos = stored_auth.find(':', start)) != std::string::npos) {
            parts.push_back(stored_auth.substr(start, pos - start));
            start = pos + 1;
        }
        parts.push_back(stored_auth.substr(start));
        if (parts.size() >= 3) {
            m_client_id     = parts[0];                 // "OrcaSlicer"
            m_client_secret = parts[1];                 // orca_xxxxxxxx
            m_birdwing_code = parts[2];                 // 32-stelliger Code
        } else if (parts.size() == 2) {
            // old format: access_token only (no refresh possible)
            m_client_id    = parts[0];
            m_access_token = parts[1];
        }
    }

    for (const auto& prefix : { "https://", "http://" })
        if (m_host.rfind(prefix, 0) == 0)
            m_host.erase(0, std::string(prefix).size());

    const size_t port_colon = m_host.find(':');
    if (port_colon != std::string::npos) {
        try { m_port = std::stoi(m_host.substr(port_colon + 1)); }
        catch (...) {}
        m_host = m_host.substr(0, port_colon);
    }

    // Flavor detection
    if (const auto* gcf = config->opt<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor"))
        m_is_birdwing = (gcf->value == gcfMakerBotBirdwing);

    // Default: Birdwing SSL on port 12309
    // Override: if user entered :2222 → Lava/Method HTTP
    if (m_port == LAVA_PORT && !m_is_birdwing) {
        // No explicit port → assume Birdwing
        m_port        = SSL_PORT_BIRDWING;
        m_is_birdwing = true;
    }
    if (m_port == SSL_PORT_BIRDWING) m_is_birdwing = true;
    if (m_port == LAVA_PORT)         m_is_birdwing = false;

    BOOST_LOG_TRIVIAL(debug) << "MakerbotLink: host=" << m_host
                             << " port=" << m_port
                             << " birdwing=" << m_is_birdwing;
}


// ── Birdwing: Raw SSL RPC ─────────────────────────────────────────────────────

bool MakerbotLink::birdwing_rpc(const std::string&    method,
                                 const nlohmann::json& params,
                                 nlohmann::json&       out,
                                 std::string&          error,
                                 int                   timeout_s) const
{
    if (m_host.empty()) { error = "No IP address configured."; return false; }

    BirdwingRpcClient client(m_host, m_port);
    if (!client.connect(error))
        return false;

    const nlohmann::json request = {
        {"jsonrpc", "2.0"},
        {"method",  method},
        {"params",  params},
        {"id",      1}
    };

    const bool ok = client.call(request, out, error, timeout_s);
    client.close();

    if (ok) BOOST_LOG_TRIVIAL(debug) << "MakerbotLink Birdwing RPC " << method << " OK";
    return ok;
}


// ── Single camera frame (YUYV) over Kaiten ────────────────────────────────────
// Verifiziert am Z18: request_camera_frame -> camera_frame-Notification ->
// 16-Byte-Header (total,width,height,format als big-endian uint32) + YUYV.
bool KaitenSession::fetch_camera_frame(int& width, int& height,
                                       std::string& yuyv_out,
                                       std::string& error, int timeout_s)
{
    if (!m_impl) { error = "session closed"; return false; }
    try {
        // send request_camera_frame
        const int req_id = m_impl->next_id++;
        const nlohmann::json req = {
            {"jsonrpc","2.0"},{"method","request_camera_frame"},
            {"params",nlohmann::json::object()},{"id",req_id}
        };
        const std::string msg = req.dump() + "\r\n";
        asio::write(m_impl->socket, asio::buffer(msg));

        // read bytes until we have seen the camera_frame notification;
        // danach kommen direkt 16 Byte Header + YUYV-Daten.
        // We use a small local read buffer over read_some.
        m_impl->socket.non_blocking(false);
        std::string buf;
        const auto t_start = std::chrono::steady_clock::now();
        auto timed_out = [&](){ return std::chrono::steady_clock::now() - t_start
                                       > std::chrono::seconds(timeout_s); };

        // helper: read one complete top-level JSON from the stream.
        auto read_json = [&](std::string& out_json)->bool {
            int depth=0; bool in_str=false, esc=false, started=false; char c;
            std::string acc;
            while (true) {
                if (timed_out()) { error="camera timeout (json)"; return false; }
                boost::system::error_code ec;
                if (!kaiten_wait_readable(m_impl->socket.native_handle(), 200)) continue; // nothing there yet
                size_t got = m_impl->socket.read_some(asio::buffer(&c,1), ec);
                if (ec == asio::error::would_block) continue; // SO_RCVTIMEO expiry, not a real error
                if (ec) { error=ec.message(); return false; }
                if (got==0) continue;
                if (!started) { if (c=='{'||c=='['){started=true;depth=1;acc.push_back(c);} continue; }
                acc.push_back(c);
                if (in_str) { if(!esc&&c=='"')in_str=false; esc=(c=='\\')&&!esc; continue; }
                if (c=='"') in_str=true;
                else if (c=='{'||c=='[') depth++;
                else if (c=='}'||c==']') { if(--depth==0){ out_json=acc; return true; } }
            }
        };
        // helper: read exactly n raw bytes.
        auto read_raw = [&](size_t n, std::string& out_raw)->bool {
            out_raw.clear(); out_raw.reserve(n);
            while (out_raw.size() < n) {
                if (timed_out()) { error="camera timeout (raw)"; return false; }
                char tmp[8192];
                size_t want = std::min(sizeof(tmp), n - out_raw.size());
                boost::system::error_code ec;
                if (!kaiten_wait_readable(m_impl->socket.native_handle(), 200)) continue; // nothing there yet
                size_t got = m_impl->socket.read_some(asio::buffer(tmp, want), ec);
                if (ec == asio::error::would_block) continue; // SO_RCVTIMEO expiry, not a real error
                if (ec) { error=ec.message(); return false; }
                out_raw.append(tmp, got);
            }
            return true;
        };

        // read JSONs until the camera_frame notification arrives (skip the request
        // result:true response beforehand).
        bool got_frame_notif = false;
        for (int i = 0; i < 20 && !got_frame_notif; ++i) {
            std::string js;
            if (!read_json(js)) return false;
            nlohmann::json j = nlohmann::json::parse(js, nullptr, false);
            if (j.is_discarded()) continue;
            if (j.value("method", "") == "camera_frame") { got_frame_notif = true; break; }
        }
        if (!got_frame_notif) { error="no camera_frame notification"; return false; }

        // 16-Byte-Header
        std::string hdr;
        if (!read_raw(16, hdr)) return false;
        auto be32 = [&](int off){
            return (uint32_t(uint8_t(hdr[off]))<<24)|(uint32_t(uint8_t(hdr[off+1]))<<16)
                  |(uint32_t(uint8_t(hdr[off+2]))<<8)|uint32_t(uint8_t(hdr[off+3])); };
        uint32_t total = be32(0); width = (int)be32(4); height = (int)be32(8);
        size_t pixlen = (total >= 16) ? (total - 16) : (size_t)width*height*2;
        if (width <= 0 || height <= 0 || pixlen != (size_t)width*height*2) {
            error = "implausible camera header"; return false;
        }
        if (!read_raw(pixlen, yuyv_out)) return false;
        return true;
    } catch (const std::exception& e) {
        error = std::string("camera exception: ") + e.what();
        return false;
    }
}


// ── Token-Refresh (HTTPS:443) ────────────────────────────────────────────────
// Fetches a fresh onetime access_token from client_secret + birdwing_code.
// Verifiziert gegen conveyor get_birdwing_token / do_auth_get('token', ...).
bool MakerbotLink::refresh_access_token(std::string& token_out, std::string& error) const
{
    if (m_host.empty()) { error = "No IP address configured."; return false; }
    if (m_client_secret.empty() || m_birdwing_code.empty()) {
        error = "Missing client_secret/birdwing_code - printer must be re-paired.";
        return false;
    }

    const std::string url = "https://" + m_host + ":443/auth?response_type=token"
        "&client_id=MakerWare&client_secret=" + Http::url_encode(m_client_secret) +
        "&context=jsonrpc&auth_code=" + Http::url_encode(m_birdwing_code);

    std::string body, err;
    bool ok = false;
    Http::get(url)
        .timeout_connect(10)
        .timeout_max(15)
        .tls_verify(false) // Z18 self-signed cert
        .on_complete([&](std::string resp_body, unsigned) { body = std::move(resp_body); ok = true; })
        .on_error([&](std::string, std::string e, unsigned) { err = e; })
        .perform_sync();

    if (!ok) { error = "Token refresh (HTTPS:443) failed: " + err; return false; }

    nlohmann::json j;
    try { j = nlohmann::json::parse(body); }
    catch (...) { error = "Unexpected token response: " + body; return false; }

    if (j.value("status", "") == "success" && j.contains("access_token")) {
        token_out = j["access_token"].get<std::string>();
        BOOST_LOG_TRIVIAL(info) << "MakerbotLink: refreshed access_token via HTTPS:443";
        return true;
    }
    error = "Token refresh rejected: " + body;
    return false;
}


// ── pending-print file: remembers the remote_path of the last uploaded ─────
// file per printer (host). The Device tab reads it on "start print".
// Liegt unter ~/.config/OrcaSlicer/makerbot_pending/<host>.txt
static std::string makerbot_pending_path(const std::string& host)
{
    const char* home = std::getenv("HOME");
    std::string base = home ? std::string(home) : std::string("/tmp");
    std::string dir = base + "/.config/OrcaSlicer/makerbot_pending";
    boost::filesystem::create_directories(dir);
    // host (IP) als Dateiname; Doppelpunkte etc. ersetzen
    std::string safe = host;
    for (char& c : safe) if (c == '/' || c == ':' || c == '\\') c = '_';
    return dir + "/" + safe + ".txt";
}

static void write_pending_print(const std::string& host, const std::string& remote_path)
{
    try {
        std::ofstream f(makerbot_pending_path(host), std::ios::trunc);
        f << remote_path;
        BOOST_LOG_TRIVIAL(info) << "MakerbotLink: pending print -> " << remote_path
                                << " (host " << host << ")";
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "MakerbotLink: could not write pending file: " << e.what();
    }
}

// ── Birdwing file upload over Kaiten (put_init/put_raw/put_term) ────────────
// Verified against a real Z18 (kaiten_upload_probe.py): JSON-RPC over 9999,
// put_raw sends bare JSON + 32KB raw bytes directly, put_term with CRC32.
bool MakerbotLink::get_camera_frame(KaitenSession& session, int& width,
                                   int& height, std::string& yuyv_out,
                                   std::string& error) const
{
    return session.fetch_camera_frame(width, height, yuyv_out, error, 8);
}

bool MakerbotLink::kaiten_upload_file(KaitenSession& session,
                                      const std::string& local_path,
                                      const std::string& remote_path,
                                      ProgressFn prg_fn, std::string& error) const
{
    namespace fs = boost::filesystem;
    boost::system::error_code fsec;
    if (!fs::exists(local_path, fsec)) {
        error = "local file missing: " + local_path;
        return false;
    }
    const uint64_t total = (uint64_t)fs::file_size(local_path, fsec);
    if (fsec) {
        error = "cannot stat " + local_path + ": " + fsec.message();
        return false;
    }
    // 6a guard: only transfer valid ZIP archives (magic "PK") to the printer.
    // Prevents raw G-code from ending up as "Print File Corrupt" (1021) on the printer.
    {
        char _magic[2] = {0, 0};
        std::ifstream _mf(local_path, std::ios::binary);
        _mf.read(_magic, 2);
        if (!_mf || _magic[0] != 'P' || _magic[1] != 'K') {
            error = "refusing to upload non-archive (no PK/ZIP header): " + local_path;
            return false;
        }
    }
    const int block_size = 32768;
    // file_id: 3 bytes base64, here constant 0 -> "AAAA" (one file per session)
    const std::string file_id = "AAAA";

    // put_init (first with length; without on -32602)
    nlohmann::json resp;
    nlohmann::json p_init = {
        {"file_path", remote_path}, {"file_id", file_id},
        {"block_size", block_size}, {"length", total}
    };
    if (!session.call("put_init", p_init, resp, error, 30)) {
        // length possibly not accepted -> retry without length
        nlohmann::json p2 = {
            {"file_path", remote_path}, {"file_id", file_id}, {"block_size", block_size}
        };
        if (!session.call("put_init", p2, resp, error, 30)) {
            error = "put_init failed: " + error;
            return false;
        }
    }

    std::ifstream f(local_path, std::ios::binary);
    if (!f) { error = "cannot open " + local_path; return false; }

    uint32_t crc = crc32(0L, Z_NULL, 0);
    uint64_t sent = 0;
    std::vector<char> buf(block_size);
    while (f) {
        f.read(buf.data(), block_size);
        std::streamsize n = f.gcount();
        if (n <= 0) break;
        const std::string block(buf.data(), (size_t)n);
        // put_raw: params [file_id, len], extra = raw bytes (without \r\n)
        nlohmann::json p_raw = nlohmann::json::array({file_id, (int)n});
        if (!session.call("put_raw", p_raw, resp, error, 30, &block)) {
            error = "put_raw failed: " + error;
            return false;
        }
        crc = crc32(crc, (const Bytef*)block.data(), (uInt)n);
        sent += (uint64_t)n;
        if (prg_fn) {
            bool cancel = false;
            prg_fn(Http::Progress(total, 0, sent, 0, ""), cancel);
            if (cancel) { error = "cancelled"; return false; }
        }
        if ((size_t)n < (size_t)block_size) break;
    }

    nlohmann::json p_term = {
        {"file_id", file_id}, {"length", sent}, {"crc", (uint64_t)crc}
    };
    if (!session.call("put_term", p_term, resp, error, 30)) {
        error = "put_term failed: " + error;
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "MakerbotLink: kaiten upload OK (" << sent << " bytes)";
    return true;
}

bool MakerbotLink::kaiten_print(KaitenSession& session,
                                const std::string& remote_path,
                                bool new_flow, std::string& error) const
{
    nlohmann::json params;
    // The official software (print_job_helper.js) ALWAYS passes to "print" only
    // the basename (path.basename(localPath)), never the full path - even in
    // new_flow. Fix for bug (4).
    params["filepath"] = boost::filesystem::path(remote_path).filename().string();
    if (new_flow) {
        params["transfer_wait"] = true;
    }
    nlohmann::json resp;
    if (!session.call("print", params, resp, error, 30)) {
        error = "print failed: " + error;
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "MakerbotLink: print started for " << remote_path;
    return true;
}

// Complete print start in the correct order (print -> put), 1:1 following
// the documented flow of the official MakerBot Print software.
bool MakerbotLink::kaiten_print_and_upload(KaitenSession& session,
                                           const std::string& local_path,
                                           ProgressFn prg_fn,
                                           std::string& error) const
{
    namespace fs = boost::filesystem;
    const std::string basename = fs::path(local_path).filename().string();
    // Fix Fehler (2): "/current_thing/" OHNE fuehrendes "/home/"
    // (makerbot-printer.js:677).
    const std::string remote_path = "/current_thing/" + basename;

    // Step 1: print FIRST, with transfer_wait=true and ONLY the basename.
    // The printer then enters "waiting for file" (+ homing if needed) and
    // actively expects the following upload - this avoids the "Press the
    // dial" safety prompt for unexpected files (bugs 1+3+4).
    {
        nlohmann::json params;
        params["filepath"] = basename;
        params["transfer_wait"] = true;
        nlohmann::json resp;
        if (!session.call("print", params, resp, error, 30)) {
            error = "print (transfer_wait) failed: " + error;
            return false;
        }
        BOOST_LOG_TRIVIAL(info) << "MakerbotLink: print(transfer_wait) ok, awaiting file "
                                << remote_path;
    }

    // Step 2: only now upload the file, to the /current_thing/ path.
    if (!kaiten_upload_file(session, local_path, remote_path, prg_fn, error)) {
        error = "file upload after print failed: " + error;
        return false;
    }

    // Step 3: build-plate confirmation like the original software.
    // The host dialog was the user's confirmation; now we tell the
    // the printer that the plate is clear, so it starts WITHOUT a wheel press.
    // hardware-documented flow: after the upload the process enters
    // step=="clear_build_plate" and offers the method "build_plate_cleared"
    // state. We wait specifically for this state (max 15s) and then send it
    // over the SAME session (the print process is tied to this connection).
    {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(15);
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            nlohmann::json si;
            std::string e2;
            if (session.call("get_system_information",
                             nlohmann::json::object(), si, e2, 10)) {
                try {
                    // Fix: session.call returns the RPC envelope {"result": {...}} -
                    // current_process is UNDER result (as everywhere else in the code).
                    const nlohmann::json& si_r =
                        (si.contains("result") && si["result"].is_object()) ? si["result"] : si;
                    if (si_r.contains("current_process")
                        && si_r["current_process"].is_object()) {
                        const auto& cp = si_r["current_process"];
                        const std::string step = cp.value("step", "");
                        bool has_method = false;
                        if (cp.contains("methods") && cp["methods"].is_array()) {
                            for (const auto& m : cp["methods"]) {
                                if (m.is_string()
                                    && m.get<std::string>() == "build_plate_cleared") {
                                    has_method = true;
                                }
                            }
                        }
                        if (step == "clear_build_plate" || has_method) {
                            ready = true;
                            break;
                        }
                    }
                } catch (...) {
                    // unerwartete Struktur -> weiter pollen
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (ready) {
            nlohmann::json bpc = {{"method", "build_plate_cleared"},
                                  {"params", nlohmann::json::object()}};
            nlohmann::json r3;
            std::string e3;
            if (session.call("process_method", bpc, r3, e3, 10)) {
                BOOST_LOG_TRIVIAL(info) << "MakerbotLink: build_plate_cleared ok"
                    " -> Druck startet ohne Raddruck";
            } else {
                BOOST_LOG_TRIVIAL(warning) << "MakerbotLink: build_plate_cleared"
                    " abgelehnt: " << e3
                    << " (Drucker erwartet evtl. manuelle Bestaetigung am Rad)";
            }
        } else {
            BOOST_LOG_TRIVIAL(warning) << "MakerbotLink: clear_build_plate-Zustand"
                " nicht erreicht (Timeout) - Drucker erwartet evtl. manuelle"
                " Bestaetigung am Rad";
        }
    }

    BOOST_LOG_TRIVIAL(info) << "MakerbotLink: print+upload complete for " << remote_path;
    return true;
}


// ── Plaintext kaiten session (port 9999) ─────────────────────────────────────

std::shared_ptr<KaitenSession> MakerbotLink::open_kaiten_session(std::string& error) const
{
    if (m_host.empty()) { error = "No IP address configured."; return nullptr; }

    // Reconnection: fetch a fresh onetime access_token PER connection
    // via HTTPS:443 from client_secret + birdwing_code (verified
    // against conveyor get_birdwing_token). The token obtained at pairing
    // is already used up and NOT reusable.
    std::string fresh_token;
    if (!m_client_secret.empty() && !m_birdwing_code.empty()) {
        if (!refresh_access_token(fresh_token, error))
            return nullptr;
    } else if (!m_access_token.empty()) {
        // old format without refresh ingredients: single attempt with the old token
        // (usually fails -> hint to re-pair).
        fresh_token = m_access_token;
    } else {
        error = "Printer not paired yet. Pair it first in Printer Settings "
                "(confirm the one-time handshake on the printer's handwheel), "
                "then reopen the Device tab.";
        return nullptr;
    }

    auto session = std::make_shared<KaitenSession>();
    if (!session->open(m_host, fresh_token, error))
        return nullptr;

    BOOST_LOG_TRIVIAL(info) << "MakerbotLink: opened plaintext kaiten session on "
        << m_host << ":" << KAITEN_PLAINTEXT_PORT;
    return session;
}


// ── Smart Extruder Detection ─────────────────────────────────────────────────
// Query the printer's kaiten RPC to detect which Smart Extruder is attached.
// Called after successful handshake.
// Maps kaiten "type_name" to our smart_extruder_type strings.
std::string MakerbotLink::get_toolhead_type(std::string& error) const
{
    nlohmann::json resp;
    // Use a short timeout – this is a quick info query
    if (!birdwing_rpc("get_system_information", nlohmann::json::object(), resp, error, 10))
        return "";

    // Response: {"result": {"toolheads": [{"type_name": "mk13", ...}], ...}}
    try {
        const auto& result = resp["result"];
        if (result.contains("toolheads") && !result["toolheads"].empty()) {
            const auto& th = result["toolheads"][0];
            if (th.contains("type_name")) {
                const std::string type_name = th["type_name"].get<std::string>();
                BOOST_LOG_TRIVIAL(info)
                    << "MakerbotLink: detected Smart Extruder type: " << type_name;
                // Normalize to our known types
                if (type_name == "mk13_impla")       return "mk13_impla";
                if (type_name == "mk13_experimental")return "mk13_experimental";
                if (type_name == "mk12")             return "mk12";
                if (type_name.rfind("mk13", 0) == 0) return "mk13"; // mk13, mk13_plus, etc.
                return type_name; // pass through unknown types
            }
        }
        // Older firmware: check "machine_info" or similar
        if (result.contains("machine_info")) {
            const auto& mi = result["machine_info"];
            if (mi.contains("toolhead_model"))
                return mi["toolhead_model"].get<std::string>();
        }
    } catch (const std::exception& e) {
        error = std::string("toolhead parse error: ") + e.what();
    }
    return "mk13"; // safe default for Birdwing printers
}

// ── Birdwing Auth Flow ────────────────────────────────────────────────────────

MakerbotLink::BirdwingAuthResult
MakerbotLink::birdwing_authorize(std::string& error_or_token,
                                  int          timeout_s) const
{
    // COMPLETE REWRITE (2026-06-18): Port 12309 and the old "authorize" RPC
    // do NOT exist in MakerBot's actual conveyor-3.10.1 source code.
    // The real protocol (confirmed from birdwing.py in the conveyor Python egg):
    //
    //   Step 1: HTTPS GET https://<printer>:443/auth?response_type=code
    //              &client_id=MakerWare&client_secret=<random>
    //              &username=OrcaSlicer&thingiverse_token=
    //           → printer blinks yellow, waits for button
    //           → {"status":"ok","answer_code":"<answer_code>"}
    //
    //   Step 2: Poll HTTPS GET https://<printer>:443/auth?response_type=answer
    //              &client_id=MakerWare&client_secret=<random>
    //              &answer_code=<answer_code>
    //           → {"answer":"pending"}  (while waiting)
    //           → {"answer":"accepted","code":"<birdwing_code>"}  (after press)
    //
    //   Step 3: HTTPS GET https://<printer>:443/auth?response_type=token
    //              &client_id=MakerWare&client_secret=<random>
    //              &context=jsonrpc&auth_code=<birdwing_code>
    //           → {"status":"success","access_token":"<token>"}
    //
    // Re-authentication reuses the stored access_token on port 9999 (KaitenSession).

    if (m_host.empty()) { error_or_token = "No IP address configured."; return BirdwingAuthResult::ConnectionFailed; }

    // Generate a client_secret once per pairing session (matches conveyor pattern)
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 35);
    const std::string chars = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string client_secret = "orca_";
    for (int i = 0; i < 16; ++i) client_secret += chars[dist(rng)];

    const std::string base_url = "https://" + m_host + ":443/auth?";
    const std::string common = "client_id=MakerWare&client_secret=" + client_secret;

    auto https_get = [&](const std::string& params, std::string& body, std::string& err) -> bool {
        const std::string url = base_url + params + "&" + common;
        bool ok = false;
        Http::get(url)
            .timeout_connect(10)
            .timeout_max(15)
            .tls_verify(false) // Z18 uses self-signed cert (same as conveyor's _ssl_kwargs)
            .on_complete([&](std::string resp_body, unsigned) {
                body = std::move(resp_body);
                ok = true;
            })
            .on_error([&](std::string /*body*/, std::string error, unsigned) {
                err = error;
            })
            .perform_sync();
        return ok;
    };

    // ── Step 1: Request a code (triggers yellow blink on Z18) ──────────────
    std::string body, err;
    const std::string code_params =
        "response_type=code&username=OrcaSlicer&thingiverse_token=";
    if (!https_get(code_params, body, err)) {
        error_or_token = "Could not reach Z18 at https://" + m_host + ":443 — " + err;
        BOOST_LOG_TRIVIAL(warning) << "MakerbotLink birdwing_authorize step1 failed: " << err;
        return BirdwingAuthResult::ConnectionFailed;
    }

    nlohmann::json j1;
    try { j1 = nlohmann::json::parse(body); } catch (...) {
        error_or_token = "Unexpected response from printer (step1): " + body;
        return BirdwingAuthResult::ConnectionFailed;
    }
    if (j1.value("status", "") != "ok" || !j1.contains("answer_code")) {
        error_or_token = "Printer rejected code request: " + body;
        return BirdwingAuthResult::ConnectionFailed;
    }
    const std::string answer_code = j1["answer_code"].get<std::string>();
    BOOST_LOG_TRIVIAL(info) << "MakerbotLink birdwing_authorize: got answer_code, waiting for button press...";

    // ── Step 2: Poll until button pressed (or timeout) ──────────────────────
    const std::string answer_params =
        "response_type=answer&answer_code=" + Http::url_encode(answer_code);

    std::string birdwing_code;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string abody, aerr;
        if (!https_get(answer_params, abody, aerr)) {
            BOOST_LOG_TRIVIAL(warning) << "MakerbotLink birdwing_authorize step2 poll failed: " << aerr;
            continue;
        }
        nlohmann::json j2;
        try { j2 = nlohmann::json::parse(abody); } catch (...) { continue; }
        const std::string answer = j2.value("answer", "");
        if (answer == "accepted") {
            birdwing_code = j2.value("code", "");
            break;
        } else if (answer == "rejected") {
            error_or_token = "Button press was rejected by the printer.";
            return BirdwingAuthResult::ConnectionFailed;
        }
        // answer == "pending" → keep polling
    }

    if (birdwing_code.empty()) {
        error_or_token = "Timed out waiting for button press on the printer.";
        return BirdwingAuthResult::Timeout;
    }

    // ── Step 3: Exchange birdwing_code for access_token ─────────────────────
    const std::string token_params =
        "response_type=token&context=jsonrpc&auth_code=" + Http::url_encode(birdwing_code);
    std::string tbody, terr;
    if (!https_get(token_params, tbody, terr)) {
        error_or_token = "Could not fetch access token from printer — " + terr;
        return BirdwingAuthResult::ConnectionFailed;
    }

    nlohmann::json j3;
    try { j3 = nlohmann::json::parse(tbody); } catch (...) {
        error_or_token = "Unexpected token response from printer: " + tbody;
        return BirdwingAuthResult::ConnectionFailed;
    }
    if (j3.value("status", "") == "success" && j3.contains("access_token")) {
        // NEW: instead of the (used-up) access_token we return the permanently
        // reusable ingredients: client_secret + birdwing_code.
        // format is stored by the dialog as printhost_password:
        //   "<client_secret>:<birdwing_code>"
        // (The dialog adds the "OrcaSlicer:" prefix in front.)
        error_or_token = client_secret + ":" + birdwing_code;
        BOOST_LOG_TRIVIAL(info) << "MakerbotLink birdwing_authorize: paired successfully (secret+code stored for reconnect).";
        return BirdwingAuthResult::Success;
    }

    error_or_token = "Token request failed: " + tbody;
    return BirdwingAuthResult::ConnectionFailed;
}




// ── Lava/Method: JSON-RPC over HTTP ──────────────────────────────────────────

bool MakerbotLink::lava_rpc(const std::string&    method,
                             const nlohmann::json& params,
                             nlohmann::json&       out,
                             std::string&          error) const
{
    if (m_host.empty()) { error = "No IP address configured."; return false; }

    const std::string url = (boost::format("http://%1%:%2%/rpc") % m_host % m_port).str();
    const nlohmann::json payload = {
        {"jsonrpc", "2.0"}, {"method", method}, {"params", params}, {"id", 1}
    };

    std::string resp_body;
    unsigned    http_status = 0;
    std::string http_error;

    auto http = Http::post(url);
    http.header("Content-Type", "application/json");
    http.header("Accept",       "application/json");
    if (!m_access_token.empty())
        http.header("Authorization", "Bearer " + m_access_token);
    http.set_post_body(payload.dump());
    http.on_complete([&](std::string body, unsigned status) {
        resp_body   = std::move(body);
        http_status = status;
    });
    http.on_error([&](std::string, std::string err, unsigned) { http_error = std::move(err); });

    try { http.perform_sync(); } catch (const std::exception& e) { error = e.what(); return false; }

    if (!http_error.empty()) { error = http_error; return false; }
    if (resp_body.empty()) {
        error = http_status >= 400
            ? (boost::format("HTTP %1%") % http_status).str()
            : "No response from MakerBot port 2222";
        return false;
    }

    try { out = nlohmann::json::parse(resp_body); }
    catch (...) { error = "JSON parse error"; return false; }

    if (out.contains("error")) { error = out["error"].value("message", "RPC error"); return false; }
    BOOST_LOG_TRIVIAL(debug) << "MakerbotLink Lava RPC " << method << " OK";
    return true;
}


// ── PrintHost Interface ───────────────────────────────────────────────────────

wxString MakerbotLink::get_test_ok_msg() const
{
    return m_is_birdwing
        ? wxString::FromUTF8("Connected to MakerBot Birdwing (SSL port 12309). Press the handwheel to authorize.")
        : wxString::FromUTF8("Connected to MakerBot Lava/Method (HTTP port 2222).");
}

wxString MakerbotLink::get_test_failed_msg(wxString& msg) const
{
    const std::string hint = m_is_birdwing
        ? "Port 12309 SSL. For Lava/Method printers add :2222 to the IP."
        : "Port 2222 HTTP. For Birdwing printers (Z18/Replicator+) remove :2222.";
    return msg.empty()
        ? wxString::FromUTF8(hint)
        : msg + wxString::FromUTF8(" — ") + wxString::FromUTF8(hint);
}

bool MakerbotLink::test(wxString& curl_info) const
{
    std::string err;

    if (m_is_birdwing) {
        // Handshake only – immediate response, no button press needed
        nlohmann::json resp;
        if (birdwing_rpc("handshake", nlohmann::json::object(), resp, err, 10))
            return true;
    } else {
        nlohmann::json resp;
        if (lava_rpc("auth.check", nlohmann::json::object(), resp, err))
            return true;
    }

    curl_info = wxString::FromUTF8(err);
    return false;
}

bool MakerbotLink::upload(PrintHostUpload upload_data,
                           ProgressFn      prg_fn,
                           ErrorFn         err_fn,
                           InfoFn          info_fn) const
{
    std::string err;

    if (m_is_birdwing) {
        // GEAENDERT (Daniel 2026-06-28): KEIN Vorab-Upload mehr. Der Upload
        // should run only AFTER the build-plate confirmation in the Device tab,
        // together with the print call in the correct order
        // (kaiten_print_and_upload). So here only the LOCAL
        // path of the finished .makerbot file is remembered as "pending", so the
        // Device tab knows at the start button which file to print.
        // We do NOT test the connection here - that happens anyway during
        // Druckstart im Device-Tab.
        namespace fs = boost::filesystem;
        const fs::path src(upload_data.source_path);
        // Persistente Ablage: PrintHost loescht source_path (transiente
        // temp file) after upload(). So we copy it to a
        // remaining .makerbot path and remember THIS as pending. The
        // name comes from upload_path (the real project name), not the
        // hidden temp name. Fallback if empty/hidden.
        std::string fname = upload_data.upload_path.filename().string();
        if (fname.empty() || fname[0] == '.')
            fname = "current_print.makerbot";
        const fs::path dst =
            fs::path(makerbot_pending_path(m_host)).parent_path() / fname;
        {
            boost::system::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            std::ifstream in(src.string(), std::ios::binary);
            std::ofstream out(dst.string(), std::ios::binary | std::ios::trunc);
            if (!in || !out) {
                err_fn("Could not stage .makerbot file at " + dst.string());
                return false;
            }
            out << in.rdbuf();
            out.flush();
            if (!out) {
                err_fn("Could not write .makerbot file at " + dst.string());
                return false;
            }
        }
        write_pending_print(m_host, dst.string());

        info_fn("", "Ready. Open the Device tab, confirm the build plate is "
                    "clear, then start the print.");
        bool cancel = false;
        prg_fn(Http::Progress(0, 0, 100, 100, ""), cancel);
        return true;
    } else {
        info_fn("", "Authenticating with MakerBot Lava/Method...");
        nlohmann::json resp;
        if (!lava_rpc("auth.check", nlohmann::json::object(), resp, err)) {
            err_fn("MakerBot auth failed: " + err);
            return false;
        }
    }

    // Upload token
    info_fn("", "Requesting upload slot...");
    const boost::filesystem::path src(upload_data.source_path.string());
    const nlohmann::json tok_params = {
        {"filename", src.filename().string()},
        {"length",   (uint64_t)boost::filesystem::file_size(src)}
    };

    nlohmann::json tok_resp;
    const bool tok_ok = m_is_birdwing
        ? birdwing_rpc("upload.request_token", tok_params, tok_resp, err, 30)
        : lava_rpc("upload.request_token", tok_params, tok_resp, err);

    if (!tok_ok) { err_fn("Upload token failed: " + err); return false; }

    const std::string uid = tok_resp["result"]["upload_id"].get<std::string>();
    const std::string schema = m_is_birdwing ? "https" : "http";
    const std::string upload_url =
        (boost::format("%1%://%2%:%3%/upload/%4%") % schema % m_host % m_port % uid).str();

    info_fn("", "Streaming .makerbot archive...");

    unsigned    http_status = 0;
    std::string upload_err;
    auto http = Http::put(upload_url);
    if (m_is_birdwing) http.tls_verify(false);
    if (!m_access_token.empty()) http.header("Authorization", "Bearer " + m_access_token);
    http.set_put_body(src);
    http.on_complete([&](std::string, unsigned s) { http_status = s; });
    http.on_error([&](std::string, std::string e, unsigned) { upload_err = std::move(e); });
    http.on_progress([&prg_fn](Http::Progress p, bool& c) { prg_fn(p, c); });

    try { http.perform_sync(); } catch (const std::exception& e) {
        err_fn(std::string("Upload error: ") + e.what()); return false;
    }
    if (!upload_err.empty()) { err_fn("Upload error: " + upload_err); return false; }
    if (http_status != 200 && http_status != 201) {
        err_fn((boost::format("HTTP %1%") % http_status).str()); return false;
    }

    info_fn("", "Upload complete.");
    bool cancel = false;
    prg_fn(Http::Progress(0, 0, 100, 100, ""), cancel);
    return true;
}

} // namespace Slic3r
