#ifndef NOTIFY_HTTP_SERVER_H
#define NOTIFY_HTTP_SERVER_H

// Notify / local MCP HTTP server —— 独立于小智 wss 通道，让 Mac 端通过局域网
// HTTP 直接驱动派蒙，不依赖云端、不依赖 Qoder、不需要本地 WebSocket gateway。
//
// 原有能力（保留，向后兼容 claude-bridge-mcp / WorkBuddy 连接器）：
//   POST /notify   {"project": "xxx"}  触发"等你确认"注意力提醒
//   POST /dismiss                      清除提醒
//   GET  /healthz                     健康检查
//
// 功能整合新增：
//   GET  /tools                       返回设备已注册的全部 MCP 工具（tools/list）
//   POST /mcp                        通用 MCP JSON-RPC 中继，同步返回结果
//                                    也接受简写 {"tool":"...","arguments":{...}}
//   GET  /config                     读 NVS 里的 websocket / ota 配置（token 打码）
//   POST /config                     写 NVS 配置，可带 {"restart":true} 立即重启
//
// 设计要点：
//  1. /mcp 把 JSON-RPC 交给 McpServer::ParseMessage()，复用设备全部工具实现；
//     McpServer 对 id >= kHttpRpcIdBase 的响应改投递给 ResultSink，由本服务
//     同步回给 HTTP 调用方。云端 wss 的小 id 完全不受影响。
//  2. /config 解决"设备怎么连本地 gateway"这个结构性卡点：WebsocketProtocol
//     从 NVS namespace "websocket" 读 url/token/version，Ota 从 "wifi" 读
//     ota_url。写进 NVS 后重启即可在小智云与自托管 gateway 之间切换，
//     不需要重新编译或擦除 Wi-Fi 配置。
//  3. 端口 8788（跟 kindle-monitor 的 8787 岔开，两套独立）。
//
// 用法（在 board 的 StartNetwork() 里）：
//    notify_http_server_ = new NotifyHttpServer();
//    notify_http_server_->Start(8788);

#include <string>

#include <esp_http_server.h>

class NotifyHttpServer {
public:
    NotifyHttpServer();
    ~NotifyHttpServer();

    bool Start(int port = 8788);
    void Stop();

private:
    httpd_handle_t server_handle_;

    // URI handlers（static，通过 user_ctx 拿实例；参照 otto ws_handler 模式）
    static esp_err_t notify_handler(httpd_req_t* req);
    static esp_err_t dismiss_handler(httpd_req_t* req);
    static esp_err_t healthz_handler(httpd_req_t* req);
    static esp_err_t tools_handler(httpd_req_t* req);
    static esp_err_t mcp_handler(httpd_req_t* req);
    static esp_err_t config_get_handler(httpd_req_t* req);
    static esp_err_t config_post_handler(httpd_req_t* req);

    // 把 POST body 完整读进 std::string；超长返回 false
    static bool ReadBody(httpd_req_t* req, size_t max_len, std::string* out);

    // 收到 POST 后把 body 里的 project 名字装进 MCP 报文，触发 self.notify.attention
    void InvokeAttention(const char* project);
    void InvokeDismiss();

    // 同步执行一次 JSON-RPC 调用，返回完整 JSON-RPC 响应字符串
    std::string CallMcpSync(const std::string& jsonrpc_request, int id);
};

#endif  // NOTIFY_HTTP_SERVER_H
