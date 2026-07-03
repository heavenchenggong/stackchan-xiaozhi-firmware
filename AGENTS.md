# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Add durable project-specific notes here as they are discovered through real work.

## LAN control channel (m5stack-core-s3)

The CoreS3 board runs an **inbound LAN WebSocket control server** *alongside* the normal
xiaozhi.me cloud voice assistant (the cloud link is untouched). It lets a LAN peer (e.g. an
agent on the Mac) push speech + expression/gesture to the robot on its own initiative, even
while the device is idle. Modeled on the in-repo `otto-robot` board.

- **Endpoint:** `ws://<device-ip>:8080/ws` (WebSocket, text frames).
- **Files:** `main/boards/m5stack-core-s3/lan_control_server.{cc,h}` (class `LanControlServer`,
  adapted from `main/boards/otto-robot/websocket_control_server.{cc,h}`). Started from
  `M5StackCoreS3Board::StartNetwork()` (override) → `InitializeLanControlServer()`, which also
  registers an `Application::RegisterMcpBroadcastCallback` so MCP replies are broadcast back to
  LAN clients as well as the cloud.
- **Build flag:** `CONFIG_HTTPD_WS_SUPPORT=y` in the board's `config.json` `sdkconfig_append`
  (required for the httpd WebSocket API). `esp_http_server` is listed in `main/CMakeLists.txt`
  `PRIV_REQUIRES`.
- **Message envelope** (same as otto accepts):
  `{"type":"mcp","payload":{"jsonrpc":"2.0","id":<n>,"method":"tools/call","params":{"name":"<tool>","arguments":{...}}}}`.
  The payload is fed straight into `McpServer::GetInstance().ParseMessage(...)` — the SAME
  dispatcher the cloud uses — so every existing device MCP tool is reachable over the LAN
  (`self.face.expression`, `self.head.nod`, `self.head.shake`, LED tools, etc.).
- **Auth (shared token) — fail-closed:** every LAN request must carry a `token` matching the
  configured secret, or it is dropped in `LanControlServer::HandleMessage` before dispatch.
  The token is read by `LanControlServer::GetConfiguredToken()`: **NVS** namespace `lan_ctrl`,
  key `token` (runtime, no reflash) takes precedence over the compile-time Kconfig default
  `CONFIG_LAN_CONTROL_TOKEN` (`main/Kconfig.projbuild`, default `""`). If the token is empty
  (nothing configured) the channel is **disabled** — all requests rejected. The token may be
  placed either at the envelope top level (`{"type":"mcp","token":"...","payload":{...}}`) or
  inside the tool `arguments` (`params.arguments.token`); the server accepts either.
  Set the runtime token by writing NVS `lan_ctrl`/`token` (e.g. from a serial console /
  `nvs_set`) — do NOT commit a real secret into Kconfig.

## `self.play_audio_url` MCP tool — the "speak" primitive

Device has **no local text-to-speech**: spoken audio must be produced elsewhere (the Mac) and
fetched. `self.play_audio_url` fetches an audio file over the board's outbound HTTP (same
pattern as `self.screen.preview_image`) and plays it through the local decode queue (same path
as `AudioService::PlaySound`), independent of the cloud session. Registered in
`M5StackCoreS3Board::RegisterPlayAudioMcpTool()` (`m5stack_core_s3.cc`), user-only (not exposed
to the conversational LLM). Sets a `speaking` emotion + one `Nod` while playing.

- **Arguments:** `{"url":"http://<mac-ip>:<port>/utt.ogg","token":"<shared>"}`. Enforces the
  token itself (defense-in-depth on top of the server gate).
- **REQUIRED AUDIO FORMAT (device is the source of truth):**
  **Ogg-encapsulated Opus, 16 kHz sample rate, mono (1 channel), 60 ms frames.**
  Rationale: the pre-baked assets under `main/assets/**/*.ogg` are all Ogg/Opus 16 kHz mono
  (verify via the `OpusHead`), `AudioService::PlaySound` hard-codes `frame_duration = 60` ms,
  and the demux→`esp_opus_dec`→playback path (`audio_service.cc` `OpusCodecTask` /
  `PlaySound`) is built around that. The Mac producer must emit exactly this, e.g.
  `ffmpeg -i in.wav -ar 16000 -ac 1 -c:a libopus -frame_duration 60 -f ogg utt.ogg`.
  Serve the file with a `Content-Length` header (the fetch pre-allocates from it).

## Flashing (OTA stripped — USB only)

This fork removed OTA, so firmware updates are a **USB `idf.py flash`** (not OTA). CI builds via
`.github/workflows/build-cores3.yaml` (ESP-IDF v5.5.2), which triggers on push to
`main`/`codex-refactor`, on `pull_request` to `main`, and `workflow_dispatch`. There is no local
ESP-IDF on the dev Mac — rely on CI for compile verification.
