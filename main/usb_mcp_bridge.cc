#include "usb_mcp_bridge.h"
#include "mcp_server.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <string>

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <unistd.h>

static const char* TAG = "UsbMcp";
static std::atomic<int> g_next_id{McpServer::kUsbRpcIdBase};

static void UsbMcpDeliverResult(int id, const std::string& payload) {
    (void)id;
    // 用固定前缀，Mac 端按行过滤，避免和 ESP_LOG 日志混在一起。
    printf("__MCP_RESULT__%s\n", payload.c_str());
    fflush(stdout);
}

static void UsbMcpSendError(const char* message) {
    printf("__MCP_RESULT__{\"jsonrpc\":\"2.0\",\"id\":0,\"error\":{\"message\":\"%s\"}}\n", message);
    fflush(stdout);
}

static void UsbMcpHandleLine(const std::string& line) {
    if (line.empty() || line[0] != '{') return;  // 不是 JSON，忽略
    cJSON* in = cJSON_Parse(line.c_str());
    if (in == nullptr || !cJSON_IsObject(in)) {
        if (in) cJSON_Delete(in);
        UsbMcpSendError("invalid JSON object");
        return;
    }

    int id = g_next_id.fetch_add(1);
    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", id);

    cJSON* method = cJSON_GetObjectItem(in, "method");
    cJSON* tool = cJSON_GetObjectItem(in, "tool");
    bool valid = false;
    if (method != nullptr && cJSON_IsString(method)) {
        cJSON_AddStringToObject(req, "method", method->valuestring);
        cJSON* params = cJSON_GetObjectItem(in, "params");
        cJSON_AddItemToObject(req, "params",
                              params != nullptr ? cJSON_Duplicate(params, true)
                                               : cJSON_CreateObject());
        valid = true;
    } else if (tool != nullptr && cJSON_IsString(tool)) {
        cJSON_AddStringToObject(req, "method", "tools/call");
        cJSON* params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "name", tool->valuestring);
        cJSON* args = cJSON_GetObjectItem(in, "arguments");
        cJSON_AddItemToObject(params, "arguments",
                              args != nullptr ? cJSON_Duplicate(args, true)
                                             : cJSON_CreateObject());
        cJSON_AddItemToObject(req, "params", params);
        valid = true;
    }

    if (valid) {
        McpServer::GetInstance().ParseMessage(req);
    } else {
        UsbMcpSendError("need method or tool");
    }
    cJSON_Delete(req);
    cJSON_Delete(in);
}

static void UsbMcpTask(void*) {
    // 阻塞读 stdin。这是独立 task，阻塞无妨；之前设 O_NONBLOCK 会让
    // ESP-IDF 的 usb_serial_jtag VFS 一直返回 EAGAIN，命令根本读不进来。
    ESP_LOGI(TAG, "USB MCP bridge ready: send one JSON line per request");

    std::string line;
    char buf[256];
    for (;;) {
        int n = (int)read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r') {
                    if (!line.empty()) {
                        UsbMcpHandleLine(line);
                        line.clear();
                    }
                } else {
                    line.push_back(c);
                    if (line.size() > 4096) line.clear();  // 防止超长行吃内存
                }
            }
        } else if (n < 0) {
            ESP_LOGW(TAG, "stdin read failed (errno=%d)", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void UsbMcpStart() {
    // 独立于 NotifyHttpServer：即使 Wi-Fi 没连、:8788 没起来，串口桥也能用。
    McpServer::GetInstance().SetUsbResultSink(&UsbMcpDeliverResult);
    xTaskCreate(&UsbMcpTask, "usb_mcp", 4096, nullptr, 2, nullptr);
}
