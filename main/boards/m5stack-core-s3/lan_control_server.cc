#include "lan_control_server.h"
#include "mcp_server.h"
#include "settings.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <sys/param.h>
#include <cstring>
#include <cstdlib>
#include <map>

static const char* TAG = "LanControl";

LanControlServer* LanControlServer::instance_ = nullptr;

LanControlServer::LanControlServer() : server_handle_(nullptr) {
    instance_ = this;
}

LanControlServer::~LanControlServer() {
    Stop();
    instance_ = nullptr;
}

std::string LanControlServer::GetConfiguredToken() {
    // NVS override (set at runtime, no reflash needed) takes precedence over the
    // compile-time Kconfig default.
    Settings settings("lan_ctrl", false);
    std::string token = settings.GetString("token", "");
    if (token.empty()) {
#ifdef CONFIG_LAN_CONTROL_TOKEN
        token = CONFIG_LAN_CONTROL_TOKEN;
#endif
    }
    return token;
}

// Extracts the shared auth token from an inbound envelope. Accepts it either at the top
// level ({"type":"mcp","token":"...","payload":{...}}) or inside the tool arguments
// (payload.params.arguments.token — the placement pinned by the interface contract for
// self.play_audio_url). Returns "" if no token field is present.
static std::string ExtractToken(cJSON* root) {
    cJSON* top = cJSON_GetObjectItem(root, "token");
    if (cJSON_IsString(top)) {
        return top->valuestring;
    }
    // Support both the full envelope ({"type":"mcp","payload":{...}}) and the bare payload.
    cJSON* payload = cJSON_GetObjectItem(root, "payload");
    cJSON* base = cJSON_IsObject(payload) ? payload : root;
    cJSON* params = cJSON_GetObjectItem(base, "params");
    if (cJSON_IsObject(params)) {
        cJSON* args = cJSON_GetObjectItem(params, "arguments");
        if (cJSON_IsObject(args)) {
            cJSON* at = cJSON_GetObjectItem(args, "token");
            if (cJSON_IsString(at)) {
                return at->valuestring;
            }
        }
    }
    return "";
}

esp_err_t LanControlServer::ws_handler(httpd_req_t *req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }

    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        instance_->AddClient(req);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);

    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
    }

    ESP_LOGI(TAG, "Packet type: %d", ws_pkt.type);

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket close frame received");
        instance_->RemoveClient(req);
        free(buf);
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        if (ws_pkt.len > 0 && buf != nullptr) {
            buf[ws_pkt.len] = '\0';
            instance_->HandleMessage(req, (const char*)buf, ws_pkt.len);
        }
    } else {
        ESP_LOGW(TAG, "Unsupported frame type: %d", ws_pkt.type);
    }

    free(buf);
    return ESP_OK;
}

bool LanControlServer::Start(int port) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 7;
    config.ctrl_port = 32769;

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = nullptr,
        .is_websocket = true
    };

    if (httpd_start(&server_handle_, &config) == ESP_OK) {
        httpd_register_uri_handler(server_handle_, &ws_uri);
        ESP_LOGI(TAG, "LAN control WebSocket server started on port %d", port);
        return true;
    }

    ESP_LOGE(TAG, "Failed to start LAN control WebSocket server");
    return false;
}

void LanControlServer::Stop() {
    if (server_handle_) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        clients_.clear();
        ESP_LOGI(TAG, "LAN control WebSocket server stopped");
    }
}

void LanControlServer::HandleMessage(httpd_req_t *req, const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        ESP_LOGE(TAG, "Invalid message: data is null or len is 0");
        return;
    }

    if (len > 4096) {
        ESP_LOGE(TAG, "Message too long: %zu bytes", len);
        return;
    }

    char* temp_buf = (char*)malloc(len + 1);
    if (temp_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return;
    }
    memcpy(temp_buf, data, len);
    temp_buf[len] = '\0';

    cJSON* root = cJSON_Parse(temp_buf);
    free(temp_buf);

    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    // ---- Shared-token auth gate (added vs the unauthenticated otto-robot original) ----
    // Fail closed: if no token is configured, the LAN control channel is disabled and every
    // request is rejected. Otherwise the request must carry a matching token.
    std::string expected = GetConfiguredToken();
    if (expected.empty()) {
        ESP_LOGW(TAG, "Rejected LAN request: no auth token configured (channel disabled)");
        cJSON_Delete(root);
        return;
    }
    std::string provided = ExtractToken(root);
    if (provided != expected) {
        ESP_LOGW(TAG, "Rejected LAN request: token mismatch");
        cJSON_Delete(root);
        return;
    }

    // 支持两种格式：
    // 1. 完整格式：{"type":"mcp","payload":{...}}
    // 2. 简化格式：直接是MCP payload对象

    cJSON* payload = nullptr;
    cJSON* type = cJSON_GetObjectItem(root, "type");

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "mcp") == 0) {
        payload = cJSON_GetObjectItem(root, "payload");
        if (payload != nullptr) {
            cJSON_DetachItemViaPointer(root, payload);
            McpServer::GetInstance().ParseMessage(payload);
            cJSON_Delete(payload);
        }
    } else {
        payload = cJSON_Duplicate(root, 1);
        if (payload != nullptr) {
            McpServer::GetInstance().ParseMessage(payload);
            cJSON_Delete(payload);
        }
    }

    if (payload == nullptr) {
        ESP_LOGE(TAG, "Invalid message format or failed to parse");
    }

    cJSON_Delete(root);
}

void LanControlServer::AddClient(httpd_req_t *req) {
    int sock_fd = httpd_req_to_sockfd(req);
    if (clients_.find(sock_fd) == clients_.end()) {
        clients_[sock_fd] = req;
        ESP_LOGI(TAG, "Client connected: %d (total: %zu)", sock_fd, clients_.size());
    }
}

void LanControlServer::RemoveClient(httpd_req_t *req) {
    int sock_fd = httpd_req_to_sockfd(req);
    clients_.erase(sock_fd);
    ESP_LOGI(TAG, "Client disconnected: %d (total: %zu)", sock_fd, clients_.size());
}

size_t LanControlServer::GetClientCount() const {
    return clients_.size();
}

struct WsBroadcastJob {
    httpd_handle_t server;
    int fd;
    char* payload;
    size_t len;
};

static void ws_broadcast_send_job(void* arg) {
    WsBroadcastJob* job = static_cast<WsBroadcastJob*>(arg);

    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = reinterpret_cast<uint8_t*>(job->payload);
    ws_pkt.len = job->len;
    ws_pkt.final = true;

    esp_err_t ret = httpd_ws_send_frame_async(job->server, job->fd, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGE("LanControl", "BroadcastMessage: send failed fd=%d err=%d", job->fd, ret);
    }

    free(job->payload);
    free(job);
}

void LanControlServer::BroadcastMessage(const std::string& message) {
    if (!server_handle_ || clients_.empty()) {
        return;
    }

    for (auto& [fd, req] : clients_) {
        WsBroadcastJob* job = static_cast<WsBroadcastJob*>(malloc(sizeof(WsBroadcastJob)));
        if (!job) {
            ESP_LOGE(TAG, "BroadcastMessage: failed to allocate job");
            continue;
        }

        job->server = server_handle_;
        job->fd = fd;
        job->len = message.length();
        job->payload = static_cast<char*>(malloc(message.length() + 1));
        if (!job->payload) {
            ESP_LOGE(TAG, "BroadcastMessage: failed to allocate payload");
            free(job);
            continue;
        }
        memcpy(job->payload, message.c_str(), message.length());
        job->payload[message.length()] = '\0';

        esp_err_t ret = httpd_queue_work(server_handle_, ws_broadcast_send_job, job);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BroadcastMessage: httpd_queue_work failed fd=%d err=%d", fd, ret);
            free(job->payload);
            free(job);
        }
    }
}
