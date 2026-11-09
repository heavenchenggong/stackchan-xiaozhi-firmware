#ifndef USB_MCP_BRIDGE_H
#define USB_MCP_BRIDGE_H

// USB Serial/JTAG 串口 MCP 通道 —— 不依赖 Wi-Fi，数据线直连就能调设备全部工具。
//
// 协议：Mac 端往串口写**一行** JSON，设备回**一行**以 __MCP_RESULT__ 开头的
// 完整 JSON-RPC 2.0 响应。其余行是普通 ESP_LOG 日志，忽略即可。
//
//   写: {"tool":"self.led.set_color","arguments":{"r":255,"g":0,"b":0}}\n
//   写: {"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}\n
//   读: __MCP_RESULT__{"jsonrpc":"2.0","id":2000000,"result":{"content":[...]}}
//
// 实现上复用 McpServer::ParseMessage()，工具执行仍在主事件循环；
// 响应经 McpServer::SetUsbResultSink() 回到串口，与 :8788 的 HTTP 桥
// （1000000 段）用不同 id 段隔离，互不干扰。

// 在 app_main() 里、Application::Run() 之前调用一次。
void UsbMcpStart();

#endif  // USB_MCP_BRIDGE_H
