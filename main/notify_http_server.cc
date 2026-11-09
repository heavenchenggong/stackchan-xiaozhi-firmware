#include "notify_http_server.h"
#include "mcp_server.h"
#include "settings.h"

#include <atomic>
#include <mutex>
#include <string>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>
#include <cstdlib>

static const char* TAG = "NotifyHttp";

// 本地 HTTP 调用专用 JSON-RPC id 段。McpServer 对这段 id 的响应改走 ResultSink，
// 云端 wss 的小 id 不受影响。
static std::atomic<int> g_http_rpc_id{McpServer::kHttpRpcIdBase};

// ── 同步结果接收（一次只允许一个 /mcp 或 /tools 在飞） ──────────────────
static std::mutex g_call_mutex;
static SemaphoreHandle_t g_reply_sem = nullptr;
static std::atomic<int> g_expected_id{-1};
static std::string g_reply_payload;

static void HttpResultSink(int id, const std::string& payload) {
    if (id != g_expected_id.load()) {
        return;  // /notify、/dismiss 等异步调用的响应，直接丢弃
    }
    g_reply_payload = payload;
    if (g_reply_sem != nullptr) {
        xSemaphoreGive(g_reply_sem);
    }
}

static int NextHttpRpcId() {
    return g_http_rpc_id.fetch_add(1);
}

static std::string JsonRpcError(int id, const char* message) {
    return std::string("{\"jsonrpc\":\"2.0\",\"id\":") + std::to_string(id) +
           ",\"error\":{\"message\":\"" + message + "\"}}";
}

static void SendJson(httpd_req_t* req, const std::string& payload) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, payload.c_str(), payload.size());
}

static void DeferredRestart(void*) {
    esp_restart();
}

NotifyHttpServer::NotifyHttpServer() : server_handle_(nullptr) {}

NotifyHttpServer::~NotifyHttpServer() {
    Stop();
}

// ── 通用工具 ───────────────────────────────────────────────────────────

bool NotifyHttpServer::ReadBody(httpd_req_t* req, size_t max_len, std::string* out) {
    out->clear();
    if (req->content_len == 0) {
        return true;
    }
    if ((size_t)req->content_len > max_len) {
        return false;
    }
    out->resize(req->content_len);
    size_t total = 0;
    while (total < out->size()) {
        int r = httpd_req_recv(req, &(*out)[total], out->size() - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            out->clear();
            return false;
        }
        total += (size_t)r;
    }
    return true;
}

// ── 原有三个接口（保持行为不变） ───────────────────────────────────────

esp_err_t NotifyHttpServer::notify_handler(httpd_req_t* req) {
    auto* self = static_cast<NotifyHttpServer*>(req->user_ctx);
    if (self == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");
        return ESP_FAIL;
    }

    std::string body;
    if (!ReadBody(req, 512, &body)) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "body too large");
        return ESP_FAIL;
    }

    std::string project = "unknown";
    if (!body.empty()) {
        cJSON* root = cJSON_Parse(body.c_str());
        if (root != nullptr) {
            cJSON* p = cJSON_GetObjectItem(root, "project");
            if (p && cJSON_IsString(p) && p->valuestring != nullptr) {
                project = p->valuestring;
                if (project.size() > 40) project = project.substr(0, 40);
            }
            cJSON_Delete(root);
        }
    }

    ESP_LOGI(TAG, "notify: project=%s", project.c_str());
    self->InvokeAttention(project.c_str());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t NotifyHttpServer::dismiss_handler(httpd_req_t* req) {
    auto* self = static_cast<NotifyHttpServer*>(req->user_ctx);
    if (self == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");
        return ESP_FAIL;
    }
    std::string sink_body;
    ReadBody(req, 512, &sink_body);  // 丢弃 body，避免连接残留

    ESP_LOGI(TAG, "dismiss");
    self->InvokeDismiss();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t NotifyHttpServer::healthz_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"service\":\"stackchan-notify\",\"mcp\":true,\"config\":true}");
    return ESP_OK;
}

// ── 新增：GET /tools —— 列出设备已注册的全部 MCP 工具 ──────────────────

esp_err_t NotifyHttpServer::tools_handler(httpd_req_t* req) {
    auto* self = static_cast<NotifyHttpServer*>(req->user_ctx);
    if (self == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");
        return ESP_FAIL;
    }
    int id = NextHttpRpcId();
    std::string request = std::string("{\"jsonrpc\":\"2.0\",\"id\":") +
                          std::to_string(id) +
                          ",\"method\":\"tools/list\",\"params\":{}}";
    SendJson(req, self->CallMcpSync(request, id));
    return ESP_OK;
}

// ── 新增：POST /mcp —— 通用 MCP JSON-RPC 中继 ─────────────────────────
//
// 接受两种 body：
//   1) 标准 JSON-RPC：{"jsonrpc":"2.0","id":1,"method":"tools/call",
//                     "params":{"name":"self.head.nod","arguments":{}}}
//      （id 会被替换成本地 HTTP 段，method / params 原样转发）
//   2) 简写：{"tool":"self.head.move","arguments":{"yaw":20,"pitch":35}}
// 也支持 method = "tools/list" / "initialize"。

esp_err_t NotifyHttpServer::mcp_handler(httpd_req_t* req) {
    auto* self = static_cast<NotifyHttpServer*>(req->user_ctx);
    if (self == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");
        return ESP_FAIL;
    }

    std::string body;
    if (!ReadBody(req, 4096, &body)) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "body too large (max 4096)");
        return ESP_FAIL;
    }

    cJSON* input = cJSON_Parse(body.c_str());
    if (input == nullptr || !cJSON_IsObject(input)) {
        if (input) cJSON_Delete(input);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON object");
        return ESP_FAIL;
    }

    int id = NextHttpRpcId();
    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(request, "id", id);

    cJSON* method = cJSON_GetObjectItem(input, "method");
    cJSON* tool = cJSON_GetObjectItem(input, "tool");

    if (method != nullptr && cJSON_IsString(method)) {
        cJSON_AddStringToObject(request, "method", method->valuestring);
        cJSON* params = cJSON_GetObjectItem(input, "params");
        cJSON_AddItemToObject(request, "params",
                              params != nullptr ? cJSON_Duplicate(params, true)
                                               : cJSON_CreateObject());
    } else if (tool != nullptr && cJSON_IsString(tool)) {
        cJSON_AddStringToObject(request, "method", "tools/call");
        cJSON* params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "name", tool->valuestring);
        cJSON* args = cJSON_GetObjectItem(input, "arguments");
        cJSON_AddItemToObject(params, "arguments",
                              args != nullptr ? cJSON_Duplicate(args, true)
                                             : cJSON_CreateObject());
        cJSON_AddItemToObject(request, "params", params);
    } else {
        cJSON_Delete(request);
        cJSON_Delete(input);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need method or tool");
        return ESP_FAIL;
    }

    char* raw = cJSON_PrintUnformatted(request);
    std::string jsonrpc_request(raw);
    cJSON_free(raw);
    cJSON_Delete(request);
    cJSON_Delete(input);

    SendJson(req, self->CallMcpSync(jsonrpc_request, id));
    return ESP_OK;
}

// ── 新增：GET /config / POST /config —— 切本地 gateway 不用重编译 ──────

static std::string BuildConfigJson() {
    Settings ws("websocket", false);
    Settings wifi("wifi", false);
    std::string url = ws.GetString("url");
    std::string token = ws.GetString("token");
    std::string ota = wifi.GetString("ota_url");

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "websocket_url", url.c_str());
    cJSON_AddBoolToObject(root, "websocket_token_set", !token.empty());
    cJSON_AddNumberToObject(root, "websocket_version", ws.GetInt("version", 0));
    cJSON_AddStringToObject(root, "ota_url", ota.c_str());
    cJSON_AddStringToObject(root, "mode", url.empty() ? "xiaozhi-cloud" : "custom-ws");
    char* raw = cJSON_PrintUnformatted(root);
    std::string result(raw);
    cJSON_free(raw);
    cJSON_Delete(root);
    return result;
}

esp_err_t NotifyHttpServer::config_get_handler(httpd_req_t* req) {
    SendJson(req, BuildConfigJson());
    return ESP_OK;
}

esp_err_t NotifyHttpServer::config_post_handler(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, 1024, &body)) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "body too large");
        return ESP_FAIL;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON object");
        return ESP_FAIL;
    }

    // 只改显式给出的字段；{"clear":true} 用于清掉自定义 WS 回到小智云。
    {
        Settings ws("websocket", true);
        cJSON* v = cJSON_GetObjectItem(root, "websocket_url");
        if (v != nullptr && cJSON_IsString(v)) ws.SetString("url", v->valuestring);
        v = cJSON_GetObjectItem(root, "websocket_token");
        if (v != nullptr && cJSON_IsString(v)) ws.SetString("token", v->valuestring);
        v = cJSON_GetObjectItem(root, "websocket_version");
        if (v != nullptr && cJSON_IsNumber(v)) ws.SetInt("version", (int32_t)v->valueint);
        cJSON* clear = cJSON_GetObjectItem(root, "clear");
        if (clear != nullptr && cJSON_IsTrue(clear)) {
            ws.EraseKey("url");
            ws.EraseKey("token");
            // Settings::EraseKey() 不会置 dirty_，析构时就不会 nvs_commit。
            // 补一次写操作确保上面两个删除落盘（version=0 等价于默认）。
            ws.SetInt("version", 0);
        }
    }
    {
        Settings wifi("wifi", true);
        cJSON* v = cJSON_GetObjectItem(root, "ota_url");
        if (v != nullptr && cJSON_IsString(v)) wifi.SetString("ota_url", v->valuestring);
    }

    cJSON* restart = cJSON_GetObjectItem(root, "restart");
    bool do_restart = (restart != nullptr && cJSON_IsTrue(restart));
    cJSON_Delete(root);

    std::string result = BuildConfigJson();
    // 把重启标记拼到响应里，方便调用方确认
    result = std::string("{\"ok\":true,\"restart\":") + (do_restart ? "true" : "false") +
             ",\"config\":" + result + "}";
    SendJson(req, result);
    ESP_LOGI(TAG, "config updated: %s", result.c_str());

    if (do_restart) {
        // 延后 800ms，让 HTTP 响应先发出去再重启
        esp_timer_create_args_t args = {};
        args.callback = &DeferredRestart;
        args.name = "http_restart";
        esp_timer_handle_t timer = nullptr;
        if (esp_timer_create(&args, &timer) == ESP_OK) {
            esp_timer_start_once(timer, 800000);
        }
    }
    return ESP_OK;
}

// ── 内部：把 HTTP 请求转成 MCP tools/call 报文 ─────────────────────────

void NotifyHttpServer::InvokeAttention(const char* project) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", NextHttpRpcId());
    cJSON_AddStringToObject(root, "method", "tools/call");
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", "self.notify.attention");
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "project", project);
    cJSON_AddItemToObject(params, "arguments", args);
    cJSON_AddItemToObject(root, "params", params);

    McpServer::GetInstance().ParseMessage(root);
    cJSON_Delete(root);
}

void NotifyHttpServer::InvokeDismiss() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", NextHttpRpcId());
    cJSON_AddStringToObject(root, "method", "tools/call");
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", "self.notify.dismiss");
    cJSON_AddItemToObject(params, "arguments", cJSON_CreateObject());
    cJSON_AddItemToObject(root, "params", params);

    McpServer::GetInstance().ParseMessage(root);
    cJSON_Delete(root);
}

// 同步执行一次 JSON-RPC 调用。工具回调由 Application 主循环执行（线程安全），
// 响应经 ResultSink 投递回本任务，用二值信号量等待，超时 8 秒。
std::string NotifyHttpServer::CallMcpSync(const std::string& jsonrpc_request, int id) {
    std::lock_guard<std::mutex> lock(g_call_mutex);
    if (g_reply_sem == nullptr) {
        return JsonRpcError(id, "mcp sink not initialized");
    }

    while (xSemaphoreTake(g_reply_sem, 0) == pdTRUE) {
        // 清掉上一次遗留的信号
    }
    g_reply_payload.clear();
    g_expected_id.store(id);

    cJSON* json = cJSON_Parse(jsonrpc_request.c_str());
    if (json == nullptr) {
        g_expected_id.store(-1);
        return JsonRpcError(id, "invalid internal JSON-RPC");
    }
    McpServer::GetInstance().ParseMessage(json);
    cJSON_Delete(json);

    if (xSemaphoreTake(g_reply_sem, pdMS_TO_TICKS(8000)) != pdTRUE) {
        g_expected_id.store(-1);
        return JsonRpcError(id, "device tool call timed out");
    }
    g_expected_id.store(-1);
    if (g_reply_payload.empty()) {
        return JsonRpcError(id, "empty tool response");
    }
    return g_reply_payload;
}

// ── Start / Stop ───────────────────────────────────────────────────────

bool NotifyHttpServer::Start(int port) {
    if (server_handle_ != nullptr) {
        ESP_LOGW(TAG, "already started");
        return true;
    }

    if (g_reply_sem == nullptr) {
        g_reply_sem = xSemaphoreCreateBinary();
    }
    // 只影响 id >= kHttpRpcIdBase 的本地调用；云端 wss 通道不变
    McpServer::GetInstance().SetResultSink(&HttpResultSink);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 4;
    config.ctrl_port = 32770;
    config.stack_size = 8192;

    if (httpd_start(&server_handle_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed on port %d", port);
        server_handle_ = nullptr;
        return false;
    }

    httpd_uri_t uri_notify = {
        .uri = "/notify", .method = HTTP_POST, .handler = notify_handler, .user_ctx = this,
    };
    httpd_uri_t uri_dismiss = {
        .uri = "/dismiss", .method = HTTP_POST, .handler = dismiss_handler, .user_ctx = this,
    };
    httpd_uri_t uri_healthz = {
        .uri = "/healthz", .method = HTTP_GET, .handler = healthz_handler, .user_ctx = this,
    };
    httpd_uri_t uri_tools = {
        .uri = "/tools", .method = HTTP_GET, .handler = tools_handler, .user_ctx = this,
    };
    httpd_uri_t uri_mcp = {
        .uri = "/mcp", .method = HTTP_POST, .handler = mcp_handler, .user_ctx = this,
    };
    httpd_uri_t uri_config_get = {
        .uri = "/config", .method = HTTP_GET, .handler = config_get_handler, .user_ctx = this,
    };
    httpd_uri_t uri_config_post = {
        .uri = "/config", .method = HTTP_POST, .handler = config_post_handler, .user_ctx = this,
    };
    httpd_register_uri_handler(server_handle_, &uri_notify);
    httpd_register_uri_handler(server_handle_, &uri_dismiss);
    httpd_register_uri_handler(server_handle_, &uri_healthz);
    httpd_register_uri_handler(server_handle_, &uri_tools);
    httpd_register_uri_handler(server_handle_, &uri_mcp);
    httpd_register_uri_handler(server_handle_, &uri_config_get);
    httpd_register_uri_handler(server_handle_, &uri_config_post);

    ESP_LOGI(TAG, "HTTP MCP server on port %d: /notify /dismiss /healthz /tools /mcp /config", port);
    return true;
}

void NotifyHttpServer::Stop() {
    if (server_handle_ != nullptr) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        ESP_LOGI(TAG, "HTTP MCP server stopped");
    }
    McpServer::GetInstance().SetResultSink(nullptr);
    if (g_reply_sem != nullptr) {
        vSemaphoreDelete(g_reply_sem);
        g_reply_sem = nullptr;
    }
}
