# YAD — Yet Another Dashboard

Zero-Qt, C++17 embedded dashboard engine and preview tool targeting:
- **ESP32** (ESP-IDF component) & **Raspberry Pi Pico 2 W** (RP2350 / Pico SDK)
- **Linux desktop** (`yad_preview` via SDL3 + `mbedtls3`)
- **Logitech G19** (`g19daemon` `yad` plugin statically linking `libyad.a`)

## Features
- Pure C++17 `RGB32` (`0xFFRRGGBB`) and `RGB565` software framebuffer
- Shared JSON layout schema (`pages`, `rows`, `columns`, `card_type`, `ninepatch`, `theme`)
- Built-in cards: `clock`, `weather`, `btop_bar`, `net_activity`, `ha_binary`/`sensor_dot`, `ha_sensor`, `placeholder`
- Pure C++ HTTP/1.1 (`Content-Length` + `chunked`) & RFC 6455 WebSocket client over non-blocking TCP / `mbedtls3`
- Multi-host named `node_exporters` (including `node_gpu_*` multi-GPU metrics) + Home Assistant WebSocket (`ws://`/`wss://`) + Linux `/proc`/NVML local fallback

## Asset & Icon Pipeline
All `.9.png` frames, SVG icons (`assets/icons/*.svg`), and Terminus bitmap glyphs are pre-baked into `src/core/generated_assets.cpp` by `tools/gen_assets.py` (using `rsvg-convert`, Python `Pillow`, and `terminus-font`).
- **Runtime:** zero icon/font file or SVG library dependencies.
- **Build time:** uses the checked-in `src/core/generated_assets.cpp` by default (`YAD_REGEN_ASSETS=AUTO` regenerates automatically when `rsvg-convert`, `Pillow`, and Terminus fonts are available, or pass `-DYAD_REGEN_ASSETS=OFF` for offline/MCU builds).

## Building on Linux
```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/yad_preview -c assets/configs/default_dashboard.json
```
