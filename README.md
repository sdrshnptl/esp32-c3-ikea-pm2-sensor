# LILYGO T-01C3 · IKEA VINDRIKTNING air quality monitor

ESP-IDF project for the ESP32-C3 on the [LILYGO T-01C3](https://lilygo.cc/en-us/products/t-01c3), reading the particulate sensor inside an IKEA VINDRIKTNING air quality monitor over UART. A built-in web UI shows live PM2.5 data, and Wi-Fi provisioning plus firmware updates live on a separate settings page.

## Layout

- `main/` — thin entry point (init, sensor start, OTA rollback confirm).
- `components/pm1006/` — receive-only UART driver for the Cubic PM1006 sensor (IKEA VINDRIKTNING `REST` pad).
- `components/wifi_manager/` — AP+STA provisioning, Wi-Fi scan, credential storage in NVS, connect and switch to STA-only.
- `components/web/` — HTTP server: dashboard (`/`), settings page (`/settings`), JSON API (`/api/data`), scan/connect endpoints, and streaming OTA upload.

## Pages

| Path | Purpose |
| --- | --- |
| `/` | Live dashboard: PM2.5 value and band, 4-minute trend, sensor/Wi-Fi/uptime/heap status. |
| `/settings` | Wi-Fi network provisioning and firmware `.bin` upload. |
| `/api/data` | JSON snapshot used by the dashboard (`pm25`, `sensor_online`, `age_ms`, `frames_ok`, `frames_bad`, `uptime_s`, `heap`, `ssid`, `rssi`, `ip`, `fw`, `idf`). |

## PM2.5 sensor wiring

The VINDRIKTNING control board exposes the PM1006 UART stream on the `REST` pad. The sensor streams 20-byte frames at 9600 baud, 8N1, and the driver only listens (no request is sent).

| Sensor board | ESP32-C3 |
| --- | --- |
| `REST` (UART TX, 5 V logic) | `GPIO9` (UART1 RX) via level shifter or divider |
| `GND` | `GND` |

The `REST` pad idles at 5 V while the ESP32-C3 GPIOs are 3.3 V and not 5 V tolerant, so a level shifter or a divider (for example 1 kΩ in series with 2 kΩ to GND) is required. GPIO9 is also the ROM boot-strap pin; the sensor line idles high, so normal boot is unaffected, but do not hold it low during reset.

The RX pin, UART port, and baud rate are configurable under `Component config → PM1006 particulate sensor (IKEA VINDRIKTNING)` in `idf.py menuconfig`. The driver validates the frame header and checksum, drops corrupt frames, and reports the age of the last valid reading; the dashboard keeps showing the last value greyed out when no frame has arrived for 30 seconds.

## Provisioning and Wi-Fi flow

The device boots in AP+STA mode. The access point password is always `password`; the network name is printed over serial (for example, `T01C3-123456`).

- No saved network: connect to the AP, open `http://192.168.4.1/settings`, pick a scanned SSID, enter its password, and connect.
- On success the device saves the credentials, joins the network, and switches to STA-only mode after a short delay.
- Saved network: the device connects automatically on boot and switches to STA-only.

To re-provision, erase NVS (`idf.py erase-flash`) or reconnect to the AP before the STA link is established.

## Build and first UART flash

```sh
idf.py set-target esp32c3
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the board's serial device (for example, `/dev/ttyACM0` or `COM5`). The board may need its BOOT button held while resetting to enter the ROM serial bootloader.

## Upload a firmware `.bin` over Wi-Fi

1. Connect to the device network (`T01C3-xxxx` / `password`) or to the configured home network.
2. Open the settings page (AP: `http://192.168.4.1/settings`; STA: `http://<device-ip>/settings`).
3. Select `build/esp32-c3-ikea-pm2-sensor.bin` and upload.

The update endpoint accepts only images that fit the inactive OTA slot and pass ESP-IDF image validation. The new image is selected only after the full image is received and verified; keep the device powered during an update.

## Partition and flash assumptions

`partitions.csv` reserves two equal 1.75 MiB OTA slots and is sized for a 4 MiB flash variant. Confirm the flash size for the exact board before flashing.
