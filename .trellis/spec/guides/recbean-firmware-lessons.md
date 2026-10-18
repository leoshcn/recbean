# RecBean firmware lessons (ESP32-C3, no PSRAM)

> From on-device bring-up of `firmware/` on 2026-10-18. Firmware rules themselves live in `firmware/AGENTS.md` and `firmware/docs/development/ai-guide*.md`; this file only records what we learned.

## LVGL must use the system heap

- Symptom: a few seconds after boot, `task_wdt` fires every 5 s with `taskLVGL` running; PC decodes to `lv_draw_add_task` → `LV_ASSERT_MALLOC` (infinite loop).
- Cause: ~120 LVGL objects filled the 32 KB builtin pool (`CONFIG_LV_MEM_SIZE_KILOBYTES`), leaving nothing for per-frame draw tasks (global rounded-corner mask + Chinese text).
- Fix: `CONFIG_LV_USE_CLIB_MALLOC=y` in `firmware/sdkconfig.defaults`. Same lesson as upstream `firmware/docs/reference/y2lin/meter-ui-smoothing-and-layout.zh_CN.md`.
- Measured heap with this config: ~46 KB free in provisioning portal, ~52 KB while recording + WebSocket, ~64 KB idle after a session; `rb_mem` logs every 10 s.

## RAM budget

- Offline ring buffer is 40 frames × 1608 B ≈ 8 s (`RB_RING_FRAMES` in `firmware/main/CMakeLists.txt`); 50 frames (10 s) left too little heap at build time. Re-evaluate with the `rb_mem` logs before raising it.

## Device test workflow on this Windows host

- IDF python: `C:/Users/leoat/.espressif/python_env/idf5.5_py3.10_env/Scripts/python.exe`; esptool 4.12; ELF addr2line under `~/.espressif/tools/riscv32-esp-elf/`.
- Board enumerates as `USB Serial Device (COMn)`, USB ID `303A:1001` (USB-Serial-JTAG). It disappears when the cable is unplugged even though the device keeps running on battery.
- Opening the port does not reset; to capture boot logs, toggle RTS and re-write DTR (`s.rts=True; s.dtr=s.dtr; …`) — Windows usbser only applies RTS after a DTR write.
- Python file writes are buffered: a long background capture is unreadable until it ends.
- Every flash needs the user's consent for that exact image hash; verify with `tools/archive_firmware.py verify <archive>` first.
- The merged image at `0x0` pads over NVS (`0x9000`), wiping Wi-Fi, server address and pairing token. When `partitions.csv`/`sdkconfig.defaults` are unchanged, flash only the archive's `FoloToy-AI-Passport.bin` at `0x10000` to keep settings — tell the user which one before asking consent.
- A single long serial capture can die when the USB port blips; reopen on `SerialException` and append to the log (the 2026-10-18 capture script lived in `firmware/build/cap.py`).
- `./deploy.sh` is not runnable from the brush shell (`%1 is not a valid Win32 application`); run it via `"C:/Program Files/Git/bin/bash.exe" ./deploy.sh`.

## Push-to-talk notes (2026-10-18, dual-mode task)

- Up-key hold detection via `BSP_BTN_RELEASE` (`BUTTON_PRESS_UP`) was reliable on the real board; the TICK-based fallback in `main.c:check_ptt_release` (up-key window `[0,150)` mV, 2 consecutive misses) never fired during acceptance.
- Note mode starts audio before the WebSocket connects; typical connect + start is ~0.8 s and ~4 frames are already buffered when `started` arrives (log `笔记开始 …，已缓冲 4 帧`). A quick tap shows only `笔记：已开始采音` with no `笔记开始` line — `start` is deferred until 600+150 ms so no server record is created.
- Heap: `min_free` dropped to ~39.5 KB after several note + meeting sessions (was ~47.8 KB at first ws connect). Still fine, but recheck before growing buffers.

## Known noise

- `i2s_common: i2s_channel_disable ... not been enabled yet` at recording start/stop is harmless (BSP disables an RX/TX channel that was never enabled); clean up later.
