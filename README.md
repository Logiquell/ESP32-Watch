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

## Build & flash (Windows, PowerShell)

```powershell
. .\env.ps1                    # load ESP-IDF v5.4
idf.py build
idf.py -p COM5 flash monitor   # use your board's COM port; Ctrl+] exits the monitor
```

If the board doesn't appear as a COM port, or flashing fails, hold **BOOT**, tap **RESET**, then release **BOOT**.

## Roadmap

- [x] Digital watch face (time, date, seconds ring)
- [ ] Wi-Fi + NTP time sync, saved to the PCF85063 RTC
- [ ] Battery % and charging status from the AXP2101
- [ ] Screen dimming / sleep and wake on raise (QMI8658)
- [ ] Swipe between screens (touch)
- [ ] Step counter
