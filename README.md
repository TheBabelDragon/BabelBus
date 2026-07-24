# BabelBus

**ESP32-P4 HDMI → CSI remote video viewer** — JPEG/MJPEG only.

No keyboard. No mouse. No USB HID gadget.

Safer by design: this is a **video-only** IP display bus. You can watch a remote machine’s screen over Ethernet, but you cannot inject input.

Based on Jonathan Rowny’s excellent [p4kvm](https://github.com/jrowny/p4kvm) proof-of-concept (Apache-2.0). All HID/keyboard/mouse paths have been deliberately removed.

## What it does

- Captures HDMI via Toshiba TC358743 → CSI on ESP32-P4
- Hardware JPEG encodes frames (RGB888 path)
- Serves a simple MJPEG multipart stream over Ethernet
- Web UI shows the live feed + JPEG quality control + optional FPS counter
- mDNS: `http://babelbus.local/`

## What it deliberately does **not** do

- No USB HID keyboard or mouse
- No WebSocket input channel
- No pointer-lock / remote control
- No H.264 (JPEG encoder only)

This makes it far safer to leave on a network: compromise of the device cannot turn into keystrokes or mouse clicks on the target machine.

## Hardware

Same as upstream p4kvm:

- ESP32-P4 module with RPi-camera-compatible CSI + Ethernet (Rev < 3 recommended for this firmware)
- Toshiba TC358743 HDMI-to-CSI adapter board

## Building

1. ESP-IDF ≥ 5.x / 6.0.1 recommended
2. `idf.py set-target esp32p4`
3. `idf.py menuconfig` → **BabelBus** menu (chip revision, GPIO, JPEG quality, Ethernet pins)
4. `idf.py build flash monitor`
5. Browse to `http://babelbus.local/` or the device’s IP

### Web UI source (optional)

```bash
cd web
npm install
npm run build
# then rebuild/flash firmware so the embedded index.html is updated
```

## Security note

Still a proof-of-concept. Do **not** expose it to the public internet. Use a VPN (Tailscale, WireGuard, etc.) if you need remote access.

## License

Apache-2.0 (same as original p4kvm). Original work © Jonathan Rowny. Modifications for BabelBus (HID removal, rename, video-only) © 2026.

## Credits

- [jrowny/p4kvm](https://github.com/jrowny/p4kvm) — the foundation
- Espressif ESP32-P4 + JPEG encoder
- Toshiba TC358743
