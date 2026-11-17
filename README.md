# ESP32-Watch

Custom firmware for the **Waveshare ESP32-S3-Touch-AMOLED-2.06** watch board, built with ESP-IDF and LVGL 9.

| Part | Chip |
|---|---|
| MCU | ESP32-S3 (8 MB octal PSRAM, 32 MB flash) |
| Display | 2.06" 410×502 AMOLED, CO5300 driver over QSPI |
| Touch | FT3168 |
| Power | AXP2101 PMU |
| IMU / RTC | QMI8658 / PCF85063 |
| Audio | ES8311 codec, ES7210 dual mics |

Waveshare's board support package (`waveshare/esp32_s3_touch_amoled_2_06`) sets up the display, touch and LVGL. It's pulled from the Espressif component registry on the first build.

## Build & flash (Windows)

Easiest: open a **cmd** shell in the project folder.

```bat
env.bat                        & rem load ESP-IDF v5.4
idf.py build
idf.py -p COM5 flash monitor   & rem use your board's COM port; Ctrl+] exits the monitor
```

From PowerShell, `. .\env.ps1` does the same, but only if script execution is allowed. Windows blocks it by default; run `cmd` first and use `env.bat` instead.

If the board doesn't appear as a COM port, or flashing fails, hold **BOOT**, tap **RESET**, then release **BOOT**.

## Wi-Fi time sync

Copy `main/wifi_secrets.example.h` to `main/wifi_secrets.h` and enter your 2.4 GHz network. That file is git-ignored. The watch syncs over NTP at boot and then every 6 hours, and keeps Wi-Fi off in between. The icon at the bottom shows the status: grey = idle / not set up, amber = syncing, green = synced, red = failed. To change the timezone (POSIX TZ string), NTP server or sync interval, run `idf.py menuconfig` and open **Watch**.

## Roadmap

- [x] Digital watch face (time, date, seconds ring)
- [x] Wi-Fi + NTP time sync (radio off between syncs)
- [ ] Save synced time to the PCF85063 RTC so it survives reboots without Wi-Fi
- [x] Battery % and charging status from the AXP2101
- [ ] Screen dimming / sleep and wake on raise (QMI8658)
- [ ] Swipe between screens (touch)
- [ ] Step counter
