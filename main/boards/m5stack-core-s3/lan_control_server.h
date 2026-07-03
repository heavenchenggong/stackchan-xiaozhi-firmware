#ifndef LAN_CONTROL_SERVER_H
#define LAN_CONTROL_SERVER_H

#include <esp_http_server.h>
#include <cJSON.h>
#include <string>
#include <map>

// Inbound LAN control channel for the M5Stack CoreS3 board.
//
// Adapted from main/boards/otto-robot/websocket_control_server.{cc,h}. It hosts an
// esp_http_server WebSocket endpoint on /ws (default port 8080) and feeds inbound LAN
// JSON straight into McpServer::GetInstance().ParseMessage(...) — the SAME dispatcher the
// xiaozhi.me cloud link uses — so it coexists with (does not disturb) the cloud voice
// assistant. MCP replies are broadcast back to connected LAN clients via a callback
// registered by the board (Application::RegisterMcpBroadcastCallback).
//
// Difference from the otto-robot original: every inbound request MUST carry a shared
// `token` matching the configured secret, or it is rejected before dispatch. See
// GetConfiguredToken().
class LanControlServer {
public:
    LanControlServer();
    ~LanControlServer();

    bool Start(int port = 8080);

    void Stop();

    size_t GetClientCount() const;

    void BroadcastMessage(const std::string& message);

    // Returns the shared auth token: NVS Settings("lan_ctrl", "token") if set, otherwise the
    // compile-time CONFIG_LAN_CONTROL_TOKEN default. Empty means the LAN channel is disabled
    // (fail-closed): every request is rejected until a token is configured. Shared by both the
    // server-side gate (HandleMessage) and the self.play_audio_url tool.
    static std::string GetConfiguredToken();

private:
    httpd_handle_t server_handle_;
    std::map<int, httpd_req_t*> clients_;

    static esp_err_t ws_handler(httpd_req_t *req);

    void HandleMessage(httpd_req_t *req, const char* data, size_t len);
    void AddClient(httpd_req_t *req);
    void RemoveClient(httpd_req_t *req);
    static LanControlServer* instance_;
};

#endif // LAN_CONTROL_SERVER_H
