# BabelBus

**ESP32-P4 HDMI → CSI remote A/V viewer** — JPEG/MJPEG video + optional I2S audio.

No keyboard. No mouse. No USB HID gadget.

Safer by design: watch (and listen to) a remote machine over the network, but you **cannot inject input**.

Based on Jonathan Rowny’s [p4kvm](https://github.com/jrowny/p4kvm) (Apache-2.0). HID paths removed; I2S audio + Wi‑Fi path added for Waveshare ESP32-P4-WIFI6 hardware.

## What it does

- HDMI capture via Toshiba TC358743 → CSI on ESP32-P4
- Hardware **JPEG** encode (UYVY 422 path)
- MJPEG multipart stream: `GET /stream`
- Optional **I2S audio** from the adapter’s flying leads: `GET /audio` (raw s16le stereo ~48 kHz)
- Ethernet (default) and optional **Wi‑Fi STA** via onboard ESP32-C6 (ESP-Hosted)
- Web UI: live video + JPEG quality + FPS
- mDNS: `http://babelbus.local/`

## What it does **not** do

- No USB HID keyboard or mouse
- No remote control / pointer lock
- No H.264

## Target hardware (tested intent)

- **Waveshare ESP32-P4-WIFI6-Dev-Kit Rev 1.1** (Ethernet + ESP32-C6 Wi‑Fi 6)
- **GODIYMODULES / BliKVM-style** HDMI→CSI adapter (TC358743, 1080p, I2S audio pads)

### Video (CSI ribbon)

Use the **larger** FPC into the P4 **MIPI-CSI** connector (Pi-camera compatible). I2C for the bridge is expected on that path (SDA=GPIO7, SCL=GPIO8).

Internal I2C pull-ups are enabled in firmware (required for many FPC / adapter combinations).

### Audio flying leads (your labels)

| Adapter pad | Wire (yours) | Signal        | Default P4 GPIO |
|-------------|--------------|---------------|-----------------|
| GND         | **Black**    | Ground        | GND             |
| OSCK        | *(empty)*    | Optional MCLK | leave open      |
| **WFS**     | **Yellow**   | I2S WS/LRCK   | **GPIO 21**     |
| **SD**      | **Blue**     | I2S DIN       | **GPIO 22**     |
| **SCK**     | **White**    | I2S BCLK      | **GPIO 20**     |

Change GPIOs under **menuconfig → BabelBus → I2S audio** if those pins conflict on your header.

I2S is enabled by default. Stream: `http://<device>/audio`

### Network

- **Ethernet:** plug RJ45 — works out of the box when `P4KVM_ETH_ENABLE` is on.
- **Wi‑Fi (C6):** P4 has no radio; Waveshare routes Wi‑Fi through **ESP32-C6 over SDIO** using ESP-Hosted.

  ```bash
  idf.py add-dependency "espressif/esp_wifi_remote"
  idf.py add-dependency "espressif/esp_hosted"
  ```

  Then menuconfig → **BabelBus → Wi‑Fi**: enable STA, set SSID/password (2.4 GHz). Uncomment those deps in `main/idf_component.yml` if you prefer the manifest.

  C6 is usually pre-flashed with ESP-Hosted slave firmware on Waveshare kits.

## Building

1. ESP-IDF ≥ 5.x / 6.0.1
2. `idf.py set-target esp32p4`
3. `idf.py menuconfig` → **BabelBus** (JPEG quality, Ethernet, Wi‑Fi, I2S GPIOs, TC358743 reset)
4. `idf.py build flash monitor`
5. Open `http://babelbus.local/` (or the printed IP)

## Endpoints

| Path            | Description                                      |
|-----------------|--------------------------------------------------|
| `/`             | Web UI (video)                                   |
| `/stream`       | MJPEG multipart                                  |
| `/jpeg-quality` | GET; `?q=1..100` sets quality                    |
| `/audio`        | Raw PCM s16le stereo (when I2S enabled)          |

## Troubleshooting the funny log

If you see something like:

```
I2C scan: 1 device(s)
  ACK at 0x18
CHIPID=0xdfdf SYS_STATUS=0xdf
HDMI lock timeout (SYS fill cannot count as lock)
csi frame wait timeout (dma_done_irqs=0)
```

that means the **TC358743 is not on the I2C bus**.

- `0x18` is the onboard ES8311 audio codec (board I2C is fine).
- `0x0F` (TC358743) is missing → all register reads return mono-fill `0xDF`, the lock detector correctly refuses to believe the “TMDS=1 SYNC=1” bits, and CSI never sees a frame.

**Checklist**

1. Reseat the **large** CSI FPC (orientation + full insertion into the MIPI-CSI connector).
2. Confirm adapter **RESETN** is wired to GPIO 23 (or set the correct GPIO in menuconfig).
3. Adapter has power (some need 5 V as well as the CSI power from the P4).
4. Rebuild with the current main (internal I2C pull-ups are enabled).

When it is healthy the scan should show **both** `0x0F` and usually `0x18`, and CHIPID will not be a repeated byte.

## Security

Still a POC. Do **not** expose to the public internet. Prefer VPN (Tailscale, WireGuard) for remote viewing over Wi‑Fi or WAN.

## License

Apache-2.0. Original © Jonathan Rowny. BabelBus modifications (HID removal, I2S audio, Wi‑Fi path, rename) © 2026.

## Credits

- [jrowny/p4kvm](https://github.com/jrowny/p4kvm)
- Espressif ESP32-P4 / JPEG / ESP-Hosted
- Toshiba TC358743
