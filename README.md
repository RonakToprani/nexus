# NEXUS Display — sci-fi animations for the Freenove 2.4" ESP32 CYD

An always-running animation engine for the Freenove FNK0114A ESP32 display
(ST7789 240x320 IPS, "Cheap Yellow Display" class), controlled from a web
portal you host anywhere on your network.

```
┌──────────────┐   GET /api/state (poll, 2.5s)   ┌──────────────────┐
│  ESP32 CYD    │ ───────────────────────────────▶ │  portal/server.py │ ◀── browser: pick preset
│  (firmware/)  │ ◀─────────────────────────────── │  (any machine)    │
└──────────────┘        {"anim":"rain",...}       └──────────────────┘
```

## Animations

**Sci-fi:** Digital Rain (katakana matrix) · Starfield Warp · Neon HUD ·
Plasma Flux · Synthwave Grid · Tactical Radar · Reactor Core · Ghost Signal ·
Ion Tunnel (hex tunnel rush) · Low Orbit (planet with day/night terminator
and night-side city lights)

**Chill / anime:** Lofi Rain (rainy night city, warm windows, cat on the
balcony railing) · Sakura Night (petals drifting past the moon) ·
Firefly Meadow · Koi Pond (koi gliding under lily pads) · Train Window
(dusk countryside rolling past rain-streaked glass) · First Snow (snowy
city, curled-up cat)

All are procedural — rendered at ~30 fps into a full-frame 8-bit palette
framebuffer, no SD card or assets needed. Speed and brightness are
adjustable from the portal.

## Portal (server)

Zero dependencies — Python 3.8+ stdlib only:

```sh
python3 portal/server.py            # serves http://0.0.0.0:8484
python3 portal/server.py --port 9000
```

Open `http://<server-ip>:8484` in a browser. The header shows whether the
display is online; the footer shows its IP and last-contact time. State
persists in `portal/state.json` across restarts.

To keep it running on a Linux server: `nohup python3 server.py &`, or a
systemd unit / `tmux`. On macOS, `launchctl` or just leave it in a terminal.

API (for scripting/automation):

```sh
curl -X POST http://<server>:8484/api/state -d '{"anim":"sakura","speed":4,"brightness":70}'
curl http://<server>:8484/api/status
```

### Custom image
The **// CUSTOM** section of the portal accepts any PNG/JPG. The browser
cover-crops it to 320x240 and the server stores it as native RGB565
(`portal/image.bin`); the display streams it directly to the panel when the
"Custom Image" preset is active. Scripts can POST raw RGB888 bytes
(320*240*3) to `/api/image`.

## Deploying the portal on a server (auto-sync with GitHub)

```sh
sudo git clone https://github.com/RonakToprani/nexus /opt/nexus
sudo cp /opt/nexus/deploy/nexus-portal.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now nexus-portal
```

Then add a cron entry so every `git push` reaches the server within 2 min
(`deploy/update.sh` pulls and restarts the portal only when HEAD changed):

```sh
sudo crontab -e
# */2 * * * * /opt/nexus/deploy/update.sh >/dev/null 2>&1
```

Point the display at the server (hold BOOT at power-up, set the portal URL) —
or edit `DEFAULT_SERVER_URL` in `firmware/src/config.h` and reflash.

## Firmware

Built with PlatformIO (LovyanGFX + WiFiManager + ArduinoJson):

```sh
cd firmware
pio run -t upload        # port is set in platformio.ini (/dev/cu.usbserial-120)
pio device monitor       # 115200 baud
```

### WiFi / portal-URL setup
- First boot (or **hold the BOOT button while powering on**): the display
  starts an access point **NEXUS-DISPLAY**. Join it, open `192.168.4.1`,
  and enter your WiFi credentials and the portal URL.
- The firmware default portal URL is in `firmware/src/config.h`
  (`DEFAULT_SERVER_URL`). If the stored URL stops answering, the firmware
  automatically falls back to the default and adopts whichever one responds.
- **Tap the screen** to cycle animations locally (the portal's next change
  takes over again).

### Hardware notes (FNK0114A)
- ST7789 on HSPI: MISO 12 / MOSI 13 / SCLK 14 / CS 15 / DC 2, inversion on,
  BGR order; backlight PWM on GPIO 21.
- XPT2046 resistive touch; the firmware only uses its PENIRQ line (GPIO 36)
  as a tap detector.
- Onboard RGB LED (22/16/17, active low) is held off.
- Pin mapping lives in `firmware/src/config.h` if your board revision differs.

### Adding an animation
1. In `firmware/src/anims.cpp`: write `myBegin()` (set the palette) and
   `myFrame()` (draw one frame into the sprite), add a row to `ANIMS[]`.
2. In `portal/server.py`: add a matching entry to `ANIMATIONS` (same `id`)
   and, optionally, a `.p-<id>` CSS preview.
