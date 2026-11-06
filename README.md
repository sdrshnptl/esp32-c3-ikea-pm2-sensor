# LILYGO T-01C3 ESP-IDF workspace

ESP-IDF project for the ESP32-C3 on the [LILYGO T-01C3](https://lilygo.cc/en-us/products/t-01c3). The initial firmware is loaded over USB-to-UART; later application images can be uploaded over Wi-Fi from a browser.

## Layout

- `main/` — thin entry point (init + OTA rollback confirm).
- `components/wifi_manager/` — AP+STA provisioning, Wi-Fi scan, credential storage in NVS, connect and switch to STA-only.
- `components/web/` — HTTP server: provisioning page, scan list, connect endpoint, and streaming OTA upload.

## Provisioning and Wi-Fi flow

The device boots in AP+STA mode. The access point password is always `password`; the network name is printed over serial (for example, `T01C3-123456`).

- No saved network: connect to the AP, open `http://192.168.4.1/`, pick a scanned SSID, enter its password, and connect.
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
2. Open the device page (AP: `http://192.168.4.1/`; STA: the IP printed over serial).
3. Select `build/esp32-c3-ikea-pm2-sensor.bin` and upload.

The OTA page accepts only images that fit the inactive OTA slot and pass ESP-IDF image validation. The new image is selected only after the full image is received and verified; keep the device powered during an update.

## Partition and flash assumptions

`partitions.csv` reserves two equal 1.75 MiB OTA slots and is sized for a 4 MiB flash variant. Confirm the flash size for the exact board before flashing.

## Current application scope

This workspace provides the board bootstrap, Wi-Fi provisioning, and the firmware-update path. It does not yet include a particulate-matter sensor driver or sampling/reporting behavior.
