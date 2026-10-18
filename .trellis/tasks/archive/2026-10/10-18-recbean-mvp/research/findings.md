# Research findings (2026-10-18)

## Hardware / firmware baseline (from D:/project/ai-passport)

- MCU: ESP32-C3, 8 MB flash, **no PSRAM** (`sdkconfig`, build dir `~/esp/ai-passport-firmware-build`). Internal RAM is the main constraint for TLS + WebSocket + audio buffers + LVGL.
- Partitions: `nvs` 0x6000 @0x9000, `factory` app 0x7f0000 @0x10000 (`partitions.csv`). No spare storage partition for local audio.
- Buttons: 3 function keys (UP/DOWN/OK) on one ADC channel, GPIO0 (`components/bsp/include/bsp_pins.h:53-66`), short press 180 ms / long press 500 ms; separate power key.
- Audio: ES8311 codec, full-duplex I2S; `bsp_audio_set_format(hz,bits,ch)` + `bsp_audio_read()` (`components/bsp/include/bsp_audio.h:21,43`). 16 kHz/16-bit/mono recording is supported by the API.
- BSP: `bsp_audio.h`, `bsp_battery.h`, `bsp_button.h`, `bsp_display.h`, `bsp_i2c.h`. Battery SOC must be shown top-right by default (ai-guide §4).
- Rules (docs/development/ai-guide.zh_CN.md): new app must have its own redesigned UI (no demo shell); LVGL access under `bsp_lvgl_lock()`; button callbacks dispatch only; Chinese text needs the font checklist; validate with `./tools/validate.sh`; flashing requires explicit per-build consent.
- Prior art: branch `feature/xhs-dashboard` already has Wi-Fi provisioning, a LAN backend in Docker Compose (`docker-compose.yml`, `backend/`), device token pairing and UDP discovery (`docs/xhs-dashboard.zh_CN.md`). Reusable pattern for Wi-Fi setup, pairing, device↔server API.
- Reference experience: `docs/reference/phoenixzhc/network-audio-streaming-and-memory.zh_CN.md` (HTTP audio streaming + memory budget on this board).

## Doubao streaming ASR (docs 6561/1354869, raw copy: `doubao-streaming-asr-raw.md`)

- Endpoints (WebSocket, binary protocol):
  - `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel` 双向流式
  - `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async` 双向流式优化版（recommended; only returns when result changes）
  - `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream` 流式输入
- Auth headers: `X-Api-App-Key`, `X-Api-Access-Key` (or new console `X-Api-Key`), `X-Api-Resource-Id`, `X-Api-Connect-Id`. Resource IDs: 2.0 `volc.seedasr.sauc.duration` / `.concurrent`; 1.0 `volc.bigasr.sauc.*`.
- Audio: `format` pcm/wav/ogg/mp3, `codec` raw/opus, `rate` **16000 only**, pcm_s16le; 200 ms packets recommended.
- **Speaker diarization is available in streaming**: `enable_speaker_info=true` + `ssd_version="200"`; only for zh-CN default model; on `bigmodel_async` requires `enable_nonstream=true`; recommended with ASR 2.0. Speaker info is attached to `definite` utterances (additions) → partial text is unlabeled until the sentence is finalized.
- Other useful: `enable_punc`, `enable_itn`, `end_window_size` (VAD sentence cut, default 800 ms).

## Implications

- Credentials should not live on the device; a backend relay (device → our server → Doubao) keeps keys server-side, lets the server persist audio + transcript for the Web editor, and reduces device TLS memory pressure (device can use plain WS on LAN).
- Real-time screen shows: in-progress partial line (no speaker yet) + finalized lines tagged 说话人1/2/...
