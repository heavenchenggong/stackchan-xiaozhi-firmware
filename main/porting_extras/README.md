# porting_extras —— 从 stackchan-mcp 抽出的可复用模块（暂存区）

来源：`kisaragi-mochi/stackchan-mcp`（`firmware/` 是 `78/xiaozhi-esp32` v2.2.6 的 subtree），
完整解析见工作区 `analysis/PORTING.md`。

**这个目录下的 `.cc` 不参与当前构建。** `main/CMakeLists.txt` 的 SOURCES 是显式清单 +
板卡目录 GLOB，不匹配 `main/porting_extras/`，所以放进来不会影响编译，也不会让
上游基线回退。它们是「搬进仓库、等接线」的素材，接法写在下面。

## 已直接落位（无需接线）

| 文件 | 状态 |
|---|---|
| `../boards/m5stack-core-s3/touch_gesture.h` | 纯 C++、无 IDF 依赖的 Si12T 三区手势状态机（TAP / STROKE / 卡死通道恢复）。已放进板卡目录，头文件不会被 GLOB 编译，随时可 `#include` |
| `../boards/m5stack-core-s3/power_save_gate.h` | 外电/电池下是否跑省电定时器的判定 + 状态去重 |
| `../boards/m5stack-core-s3/PORTING_NOTES.md` | 上游板卡的硬件踩坑记录（SCS0009 启动延迟、boot 抬头撞限位、Si12T 误触发等） |

## 暂存、需要接线才生效

| 路径 | 用途 | 激活步骤 |
|---|---|---|
| `feetech_scs/` | MIT 版 SCS0009 舵机驱动（**不要**碰 GPL-3.0 的 `SCServo_lib`/`SCS.*`） | 整个目录移到仓库根 `components/feetech_scs/`，IDF 会自动发现；`CMakeLists.txt` 只需 `driver`。注意：仓库根 `.gitignore` 有 `components/` 规则，所以暂存时**不能**放在任何叫 `components/` 的子目录里，否则会被静默忽略 |
| `extras/listening_profile/` | listen / beat 采集 profile 解析 | 把两个 `.cc` 加进 `main/CMakeLists.txt` 的 SOURCES，并在调用方 include 对应 `.h` |
| `extras/mdns/` | gateway 自动发现（`_stackchan-mcp._tcp`） | ① `main/idf_component.yml` 加 `espressif/mdns: ^1.8.2`；② `main/Kconfig.projbuild` 加 `CONFIG_STACKCHAN_MDNS_DISCOVERY`；③ 四个文件加进 SOURCES；④ 在 `WebsocketProtocol::OpenAudioChannel()` 里接候选 URL |

## 明确不搬

- `SCS.*` / `SCSCL.*` / `SCSerial.*` / `INST.h` / `SCServo.h`：GPL-3.0，会污染本仓库 MIT 许可证。
- `boards/stackchan/stackchan.cc`（8430 行）：整板卡实现，若要做「并列第二板卡」，
  从 `analysis/porting/boards/stackchan/` 复制并加 `CONFIG_BOARD_TYPE_STACKCHAN` 构建分支，
  **不要**替换现有 `m5stack-core-s3`。
- `application.cc` / `websocket_protocol.cc` / `audio_service.cc` 全文：大部分 diff 是
  v2.2.6 与本 fork 基线之间的版本差，整文件覆盖等于把上游代码回退。
