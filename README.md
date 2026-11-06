# LILYGO T-01C3 ESP-IDF workspace

This project targets the ESP32-C3 on the [LILYGO T-01C3](https://lilygo.cc/en-us/products/t-01c3). The initial firmware is loaded over the USB-to-UART interface. Subsequent application images can be uploaded over Wi-Fi from a browser.

## What is included

- ESP-IDF application configured for `esp32c3`.
- Two OTA application slots, OTA metadata, NVS, and PHY calibration partitions.
- WPA2-protected setup access point with a per-device random password stored in NVS.
- Embedded browser page that streams a selected `.bin` image into the inactive OTA slot.
- OTA image validation, boot-slot selection, and bootloader rollback support.

The access point is intentionally standalone: it does not join a home Wi-Fi network. This avoids embedding or provisioning network credentials and lets the first firmware run without additional setup.

## Build and first UART flash

Use the ESP-IDF environment configured by the ESP-IDF VS Code extension or the project dev container. From the project root:

```sh
idf.py set-target esp32c3
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the board's serial device (for example, `/dev/ttyACM0` or `COM5`). The first flash writes the bootloader, partition table, and application. The board may need its BOOT button held while resetting to enter the ROM serial bootloader.

## Upload a firmware `.bin` over Wi-Fi

1. Leave the serial monitor open after boot. It prints a network name such as `T01C3-123456` and its WPA2 password. Save the password; it remains the same after reboots.
2. Connect a computer or phone to that network.
3. Open `http://192.168.4.1/` and select `build/esp32-c3-ikea-pm2-sensor.bin`.
4. Wait for the upload to finish and the board to reboot. The saved access-point password remains valid.

The OTA page only accepts images that fit in the inactive OTA partition and pass ESP-IDF image validation. The new firmware is selected only after the complete image has been received and verified. Keep the device powered during an update.

## Partition and flash assumptions

`partitions.csv` reserves two equal 1.75 MiB OTA application slots and is sized for a 4 MiB flash variant. Confirm the flash size for the exact board/module before flashing; using this table on a smaller-flash variant is not supported.

## Current application scope

This workspace prepares the board, UART bootstrap, and Wi-Fi firmware-update path. It does not yet include a particulate-matter sensor driver or sensor sampling/reporting behavior.
