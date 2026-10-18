<p align="right">
  <a href="UPSTREAM.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Upstream baseline

RecBean firmware is derived from the FoloToy AI Passport firmware template.

| Item | Value |
| --- | --- |
| Source repository | FoloToy AI Passport firmware (local checkout `ai-passport`, upstream `https://github.com/FoloToy/ai-passport`) |
| Branch / commit | `main` at `0b9e4c81ee4421c0bac39ca3561d65a8285acd4a` (2026-09-28, "Merge pull request #91") |
| Import date | 2026-10-18 |
| Import method | `git archive 0b9e4c8` (committed tree only; the local working tree and other branches were not copied) |
| License | See [`LICENSE`](LICENSE) (unchanged) |

## Imported unchanged

- `components/bsp/` (display, LVGL port, buttons, ES8311 audio, CW2017 battery, shared I2C) and `partitions.csv`.
- `tools/` (`check_repo.py`, `verify_firmware.py`, `archive_firmware.py`, skill installer), `tests/` BSP host tests and stubs.
- `docs/` (development, hardware design, reference, contribution, brand), `assets/images/`, `skills/`, `.github/`, `AGENTS.md`, `CLAUDE.md`.
- `CMakeLists.txt` (project name kept, so artifacts are still named `FoloToy-AI-Passport*.bin`).

`.github/workflows/` are kept for the repository gate (`actionlint`); GitHub does not run them from the `firmware/` subdirectory of the RecBean repository.

## Removed

- The hardware-test demo application: `main/main.c` (menu), `main/demo_*.c/h`, `main/demo_navigation.*`, `main/ui_pixel*.c/h`. RecBean must not reuse the demo menu or visual shell (mandatory UI redesign rule).
- Demo host tests and stubs: `tests/test_ui_pixel_math.c`, `tests/test_demo_navigation.c`, `tests/test_demo_*_runtime.c`, `tests/demo_stubs/`.

## Modified

- `main/`: new RecBean application (`main.c`, `rb_*.c/h`), new `main/idf_component.yml` (`espressif/esp_websocket_client`, LVGL pinned to 9.5.0) and `dependencies.lock`.
- `sdkconfig.defaults`: Bluetooth disabled, Montserrat 28/40, UTF-8 and glyph placeholder explicit, LVGL pool 32 KB, larger HTTP header limit for the provisioning page, and IRAM/heap budget options (see below).
- `tools/validate.sh`: runs the RecBean host tests (`tests/test_rb_*.c`, `tests/test_recbean_font.py`) instead of the removed demo tests; checks that `tests/fixtures/adpcm_vectors.json` matches `server/tests/fixtures/adpcm_vectors.json` when the server is present.
- `tools/check_repo.py`: `UPSTREAM.md` / `UPSTREAM.zh_CN.md` allowed at the root.
- `tests/test_deep_sleep_contract.py`: removed the check that read `main/demo_low_power.c`; the BSP deep-sleep contracts are unchanged.
- `docs/development/engineering/coding-conventions*.md`: the link to the removed `main/ui_pixel.c` became plain text.
- Added `assets/fonts/recbean_font_16.c`, its character inventory, and `tools/gen_recbean_font.py` (see [`assets/README.md`](assets/README.md)).

## Internal RAM budget

ESP32-C3 has no PSRAM and IRAM shares SRAM with DRAM. Static usage reported by `idf.py size` for this build:

| Build | Static D/IRAM used | Left for heap |
| --- | --- | --- |
| Upstream demo (`0b9e4c8`) | 191 386 B | 129 910 B |
| RecBean, first build (50-frame ring, default IRAM placement) | 270 422 B | 50 874 B |
| RecBean, delivered (40-frame ring, IRAM options below) | 209 734 B | 111 562 B |

The 50 KB heap left by the first build cannot hold Wi-Fi, the WebSocket client, the 19.2 KB LVGL draw buffer, the I2S DMA buffers, and the task stacks. The delivered build moves Wi-Fi hot paths, heap functions, the SPI master ISR, and logging out of IRAM (about 37 KB); reads PCM in 20 ms chunks (6.4 KB less `.bss`); and lowers the offline ADPCM ring from 50 frames (80.4 KB, 10 s) to **40 frames (64.3 KB, 8 s)** via `RB_RING_FRAMES` in `main/CMakeLists.txt`. Free heap and the largest free block are logged (`rb_mem` tag) at boot, Wi-Fi init, Wi-Fi up, WebSocket connect, recording start, every 10 s while recording, and session end. If those device measurements show enough headroom, set `RB_RING_FRAMES=50`.
