# Hardware

Board: **Waveshare ESP32-S3-Touch-AMOLED-2.06**, a watch-format development board. All parts below are on the board; this project adds no external wiring. Pin numbers are taken from the Waveshare BSP header `managed_components/waveshare__esp32_s3_touch_amoled_2_06/include/bsp/esp32_s3_touch_amoled_2_06.h` (BSP 2.0.0). Items marked **[confirm]** come from the vendor's description, not from anything this firmware reads or tests.

## Components

| Part | Function | Interface | Address | Used by this firmware |
|---|---|---|---|---|
| ESP32-S3 (rev v0.2) | MCU, dual-core Xtensa LX7, Wi-Fi 2.4 GHz, BLE | — | — | yes |
| Octal PSRAM, 8 MB (AP Memory, 3 V) | frame/draw buffers, large LVGL allocations, code and rodata | OPI, 80 MHz | — | yes |
| SPI flash, 32 MB (GigaDevice) | firmware, NVS, core dump | QIO, 80 MHz | — | yes, configured as 16 MB |
| CO5300 AMOLED controller, 2.06" 410×502 | display | QSPI (SPI2) | — | yes, through BSP's SH8601-compatible driver |
| FT3168 | capacitive touch | I²C + INT | **[confirm]** | yes, for activity/wake only |
| AXP2101 | PMU: battery charger, fuel gauge, power rails, power key | I²C | 0x34 | yes, own register driver (`battery.c`) |
| PCF85063 | real-time clock, battery-backed | I²C | 0x51 | yes, own driver (`rtc.c`) |
| QMI8658 | 6-axis IMU | I²C | **[confirm]** | no |
| ES8311 | audio codec (playback/record) | I²S + I²C | — | no |
| ES7210 + 2 microphones | microphone ADC | I²S + I²C | — | no |
| Audio amplifier | speaker output, enable on GPIO46 | GPIO | — | no |
| microSD (SDMMC 1-bit) | storage | SDMMC | — | no |
| Li-ion battery | supply, charged by the AXP2101 | — | — | yes (monitored). Capacity **[confirm]** |

Flash and PSRAM details come from the boot log: "Detected size(32768k)", "Found 8MB PSRAM device", vendor 0x0d (AP).

## Pin assignments

| Signal | GPIO | Notes |
|---|---|---|
| I²C SDA | 15 | shared bus: AXP2101, PCF85063, touch, IMU, codecs; firmware devices use 400 kHz |
| I²C SCL | 14 | |
| LCD CS | 12 | QSPI |
| LCD PCLK | 11 | QSPI clock |
| LCD DATA0–3 | 4, 5, 6, 7 | QSPI data |
| LCD RST | 8 | |
| LCD backlight | — | none; AMOLED brightness is set by panel command |
| Touch RST | 9 | |
| Touch INT | 38 | falling-edge interrupt |
| BOOT button | 0 | active low, internal pull-up; used as sleep/wake toggle |
| I²S MCLK | 16 | audio, unused |
| I²S SCLK | 41 | audio, unused |
| I²S LRCLK | 45 | audio, unused |
| I²S DOUT | 40 | audio, unused |
| I²S DIN | 42 | audio, unused |
| Power amp enable | 46 | audio, unused |
| SD CLK / CMD / D0 | 2 / 1 / 3 | unused |

The PWR button is the AXP2101 power key **[confirm]**. It turns the watch on, and on after an AXP2101 soft power-off. In testing, it was also used to power-cycle the board.

### Pins kept connected during light sleep

Tickless idle on the ESP32-S3 enables `PM_SLP_DISABLE_GPIO`, a chip errata workaround that isolates every GPIO during light sleep. `screen.c` exempts the following pins with `gpio_sleep_sel_dis()`. Without this, the touch interrupt produced false wakes:

LCD RST (8), touch RST (9), LCD PCLK (11), LCD CS (12), touch INT (38), I²C SDA (15), I²C SCL (14).

## AXP2101 registers used (`main/battery.c`)

| Register | Use |
|---|---|
| 0x00 STATUS1 | bit 5 VBUS good, bit 3 battery present |
| 0x01 STATUS2 | bits 6:5 current direction (01 = charging), bits 2:0 charge state (4 = done) |
| 0x10 COMMON_CFG | bit 0 soft power-off |
| 0x18 gauge control | bit 3 fuel-gauge enable |
| 0x30 ADC control | bit 0 battery-voltage ADC enable |
| 0x34/0x35 | battery voltage, 14 bits, mV |
| 0xA4 | fuel-gauge percentage, 0–100 |

The firmware only enables the gauge and ADC, and sets the power-off bit. It does not change charger settings or rail voltages; the BSP and the PMU defaults handle those.

## PCF85063 usage (`main/rtc.c`)

- Time is stored as **UTC**, in BCD registers 0x04–0x0A. The timezone is applied by the C library, so DST changes never write the RTC.
- If the oscillator-stop flag (bit 7 of register 0x04) is set, or the date is implausible, the reading is rejected.
- 24-hour mode is enforced through register 0x00.

## AMOLED panel commands (`main/screen.c`)

The panel is driven by the BSP. For screen-off, the firmware sends these commands itself, using the QSPI command framing `(0x02 << 24) | (cmd << 8)`:

| Step | Command |
|---|---|
| Sleep | DISPLAY OFF (0x28), SLEEP IN (0x10) |
| Wake | SLEEP OUT (0x11), 120 ms settle, DISPLAY ON (0x29) |

The firmware also enforces at least 120 ms between SLEEP IN and SLEEP OUT.
