# ESP32-Watch

Custom firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.06 watch board, written in C with ESP-IDF v5.4 and LVGL 9.5. The firmware is a working watch: it shows the time and battery state and keeps time with a battery-backed RTC and NTP. Most of the work went into power management: the standby drain was measured, diagnosed and reduced from about 10–12 %/h to about 3.3 %/h.

More detail is in [docs/](docs/):
- [hardware.md](docs/hardware.md): parts, buses and pin assignments
- [architecture.md](docs/architecture.md): firmware modules and data flow
- [development.md](docs/development.md): development log, decisions and limitations

## 1. Project Overview

The board has an ESP32-S3, a 410×502 AMOLED panel, capacitive touch, an AXP2101 power-management IC, a PCF85063 real-time clock, an IMU and an audio codec. Waveshare's board support package (BSP) brings up the display, touch and I²C bus. Everything else is in this repository:
- a register-level AXP2101 battery monitor and power-off
- a PCF85063 RTC driver
- Wi-Fi/NTP time synchronisation
- screen sleep/wake with panel sleep
- light-sleep power management
- on-device diagnostics
- two watch faces

The project is a learning and portfolio project. Every feature listed as implemented below has been run on the hardware. The [Current Development Status](#7-current-development-status) section separates what was tested from what wasn't.

## 2. Key Features

Implemented and run on hardware:

- **Timekeeping:**
  - At boot, time comes from the PCF85063 RTC. If the RTC lost power, the build timestamp is used until the next sync.
  - NTP sync over Wi-Fi writes the result back to the RTC.
  - The RTC stores UTC. The local timezone is a POSIX TZ string set in menuconfig.
- **Wi-Fi time sync only while charging:**
  - The radio is switched on only for a sync and off afterwards.
  - It syncs once after USB is plugged in, then every 6 h (configurable) while plugged in.
  - On battery the radio stays off, unless the RTC had no valid time at boot.
- **Battery monitoring (AXP2101 over I²C):**
  - percentage from the PMU fuel gauge, battery voltage, USB present, charging, full
  - read every 5 s
- **Low-battery shutdown:**
  - Triggers at ≤ 3 % or < 3.35 V on two consecutive readings, on battery only.
  - Shows "LO", then switches the watch off through the AXP2101.
  - Also checked at boot, before the display starts.
- **Screen sleep/wake:**
  - Fades out after 30 s without touch (configurable).
  - Wakes on touch or the BOOT button.
  - While dark, the AMOLED controller is put into SLEEP IN, not just set to brightness 0.
- **CPU power management:**
  - Dynamic frequency scaling between 40 and 240 MHz.
  - Automatic light sleep with tickless FreeRTOS idle while the screen is off.
- **Diagnostics:**
  - Logged every 60 s: reset reason, restart counter, PM lock and sleep-mode statistics, battery drain per unplugged run, and Wi-Fi attempts and radio-on time.
  - A copy of these statistics is kept in RTC memory and printed after the next restart.
  - Crash dumps are saved to a flash partition.
- **Two watch faces** (chosen at compile time):
  - A split-flap clock with an animated flip and glowing numerals.
  - A seven-segment "LCD" face with a weekday, battery bars and charge/sync indicators.

## 3. Hardware Components

| Function | Part | Used by this firmware |
|---|---|---|
| MCU | ESP32-S3 (dual-core Xtensa LX7, 240 MHz), 8 MB octal PSRAM, 32 MB flash | yes; flash configured as 16 MB |
| Display | 2.06" AMOLED, 410×502, CO5300 controller, QSPI | yes (via BSP; panel sleep commands sent directly) |
| Touch | FT3168 capacitive touch, I²C + interrupt | yes, for wake/activity only |
| PMU | X-Powers AXP2101, I²C 0x34 | yes, own register-level driver |
| RTC | NXP PCF85063, I²C 0x51 | yes, own driver |
| IMU | QMI8658 6-axis | not yet |
| Audio | ES8311 codec, ES7210 microphone ADC, two microphones | not used |
| Button | BOOT (GPIO0) | yes, sleep/wake toggle |

Pin assignments and bus details are in [docs/hardware.md](docs/hardware.md).

## 4. Software and Development Tools

- **Framework:** ESP-IDF v5.4 (FreeRTOS, esp_pm, esp_wifi, esp_netif SNTP, I²C master driver, esp_lcd).
- **Graphics:** LVGL 9.5.0 through `esp_lvgl_port` 2.9.0. TrueType rendering uses LVGL's Tiny TTF with the Ubuntu Medium font that ships in the LVGL package.
- **Board support:** `waveshare/esp32_s3_touch_amoled_2_06` 2.0.0, from the Espressif component registry. Exact versions are pinned in `dependencies.lock`.
- **Language:** C (C11/gnu17 as set by ESP-IDF).
- **Host:** Windows 11 with `idf.py` (CMake/Ninja). The helper scripts `env.bat` and `env.ps1` load the ESP-IDF environment.
- **Debugging:** serial log over USB-Serial-JTAG, PM profiling (`CONFIG_PM_PROFILING`), core dump to flash (`idf.py coredump-info`).
- **Development assistant:** Claude Code. See the [AI-assisted development](#ai-assisted-development) note below.

## 5. System Architecture

```
            ┌──────────── ESP32-S3 ─────────────────────────────────────┐
            │                                                           │
 BOOT ─GPIO0┤ screen.c ── sleep/wake, fade, PM locks, panel SLEEP IN/OUT │
 Touch INT ─┤    │                                                      │
            │    ▼                                                      │
            │ LVGL task (esp_lvgl_port) ── face_flip.c / face_lcd.c ──QSPI┼─► CO5300 AMOLED
            │    ▲   face_tick 250 ms        battery_tick 5 s            │
            │    │                              │                       │
            │ main.c ── boot sequence, diagnostics, low-battery logic    │
            │                                   │                       │
            │ battery.c ───────────── I²C ──────┼───────────────────────┼─► AXP2101 (0x34)
            │ rtc.c ───────────────── I²C ──────┼───────────────────────┼─► PCF85063 (0x51)
            │ wifi_time.c (own task, 60 s check) ── Wi-Fi/SNTP           │
            └───────────────────────────────────────────────────────────┘
```

- `main.c` runs the boot sequence:
  1. reset reason
  2. battery check
  3. time from the RTC
  4. display start
  5. watch face and timers
  6. screen module
  7. Wi-Fi sync task
- All drawing happens in the LVGL task, from two LVGL timers:
  - `face_tick` (250 ms) updates the face from the system clock.
  - `battery_tick` (5 s) reads the PMU, tracks drain, saves diagnostics and handles low battery.
- `screen.c` polls the BOOT button and LVGL's inactivity time every 100 ms. It holds a "no light sleep" lock only while the screen is on.
- `wifi_time.c` runs in its own FreeRTOS task. It decides once a minute whether a sync is due.

Full module descriptions are in [docs/architecture.md](docs/architecture.md).

## 6. Technical Challenges and Solutions

Each item is documented in the commit history (hash in brackets). Details are in [docs/development.md](docs/development.md).

| Problem observed | Cause found | Solution |
|---|---|---|
| Parts of the screen intermittently not drawn; log showed `spi transmit (queue) color failed` | The BSP's 410×100 draw buffer sits in PSRAM. Each flush needs a contiguous 82 KB internal DMA bounce buffer, and the largest free block was 81,920 bytes. | Two earlier attempts failed. One had no effect because the BSP ignored the flags. The other left stale pixels. Final fix: BSP draw buffer reduced to 20 lines, 16 KB per flush. [6fb8c41, 3bbb1da, 5c8b10f] |
| Watch ran flat overnight, then reset every time the screen tried to wake | The CPU stayed at 240 MHz with the screen dark. At low charge the voltage dropped under load and the chip browned out. | Light sleep while the screen is off. Controlled power-off at ≤ 3 % / < 3.35 V, also checked at boot. [836a832] |
| On battery, the screen went dark and immediately woke again | On the ESP32-S3, tickless idle isolates all GPIOs during light sleep (chip errata workaround). Isolating the touch interrupt and I²C lines produced a false touch. | Keep the LCD control, touch INT and I²C pins connected in sleep (`gpio_sleep_sel_dis`). Verified with 0 restarts over ~2 h. [83f89ae] |
| ~10–12 %/h battery drain | `pdMS_TO_TICKS(6 h)` overflowed 32 bits to ~125 s, so Wi-Fi synced every 2 min. Separately, the LVGL port's 5 ms tick timer woke the CPU 200 times a second. | 64-bit tick conversion. LVGL's clock now reads `esp_timer` instead of a periodic tick. [9a10a08] |
| Drain stayed ~8 %/h after that | The panel controller kept running at brightness 0. The CPU was held at 240 MHz whenever the screen was on. Wi-Fi synced on battery. | Panel DISPLAY OFF + SLEEP IN while dark. Frequency left to DFS. Wi-Fi sync only while charging. [1f6996b] |
| No serial log after light sleep, so tests on battery couldn't be observed | USB-Serial-JTAG stops responding after light sleep until the chip is reset | Diagnostics are kept in `RTC_NOINIT` memory and printed after a USB-triggered reset. [83f89ae, 9a10a08] |
| Linker error: multiple definition of `rtc_init` | ESP-IDF already defines `rtc_init()` | The RTC driver API is prefixed `ext_rtc_*`. [3bbb1da] |

## 7. Current Development Status

Measured on hardware (single runs, AXP2101 fuel gauge with 1 % resolution):

| Firmware | Condition | Result | Source |
|---|---|---|---|
| Before 9a10a08 | on battery | ~10–12 %/h | commit 9a10a08 |
| 9a10a08 | on battery | ~8 %/h, light sleep 88 % of the time, 1 Wi-Fi sync | commit 9a10a08 |
| 1f6996b | on battery, not touched | 100 % → 87 % in 236 min (≈ 3.3 %/h), light sleep 97 %, 208,254 light-sleep entries, 0 rejected, 0 Wi-Fi syncs on battery | boot-log diagnostics, commit 1f6996b |

At 3.3 %/h, a full charge would last about 30 h. That figure is extrapolated: a full discharge has not been measured. The battery current has not been measured directly. All figures come from the PMU's fuel gauge.

Verified on hardware (recorded in commit messages):
- NTP sync about 3 s after boot.
- RTC within 1 s of NTP after a reboot, and after a ~90 s power-off.
- No SPI errors and no leftover pixels across repeated wakes.
- Sleep/wake by timeout, touch and BOOT on battery.
- The flip animation.

Not yet verified:
- **The low-battery power-off path.** The logic is implemented, but the battery has not been run down to the threshold on this firmware.
- **RTC drift over a full day without a sync.**

## 8. Planned Improvements

Not implemented yet:

- Raise-to-wake and step counting using the QMI8658's built-in motion detection.
- Swipe navigation between screens, and an on-watch settings menu. Settings are currently compile-time options in menuconfig.
- Further standby reduction: identify which board peripherals draw current while the ESP32-S3 sleeps, and switch off unused rails through the AXP2101.
- Direct current measurement (USB power meter or shunt) to back up the fuel-gauge figures, and a full-discharge battery-life test.
- Further features such as a stopwatch, an alarm and weather.

## 9. Building and Flashing

Requirements: ESP-IDF v5.4 installed in `%USERPROFILE%\esp\v5.4\esp-idf` (the default location of the Windows installer), and the board connected over USB-C.

1. Configure Wi-Fi (optional). Copy `main/wifi_secrets.example.h` to `main/wifi_secrets.h` and enter a 2.4 GHz network. The file is git-ignored. Without it, the watch builds and runs with Wi-Fi sync disabled.
2. Open a **cmd** shell in the project folder and build:

   ```bat
   env.bat                        & rem load ESP-IDF v5.4
   idf.py build
   idf.py -p COM5 flash monitor   & rem use your board's COM port; Ctrl+] exits the monitor
   ```

   From PowerShell, `. .\env.ps1` does the same, if script execution is allowed. Windows blocks scripts by default, so use `cmd` and `env.bat`.
3. Configure the watch with `idf.py menuconfig` → **Watch**:
   - watch face
   - screen timeout
   - brightness
   - timezone (POSIX TZ string)
   - NTP server
   - resync interval while charging
4. If the board doesn't appear as a COM port, or flashing fails, hold **BOOT**, tap **RESET**, then release **BOOT**. After the watch has been in light sleep on battery, USB serial may not respond until the watch is reset or power-cycled with **PWR**.

Reading saved diagnostics after a test on battery: plug in USB and reset the watch. The boot log then prints the statistics saved before the reset. A full power-on (PWR) clears them.

## 10. Project Images

No photos or screenshots are in the repository yet. Planned: `docs/images/` with photos of both watch faces and an annotated log of the power-profiling output.

## AI-assisted development

This project was developed with Claude Code (an AI coding assistant) as a development assistant. Commits it contributed to carry a `Co-Authored-By: Claude` trailer. Most of the source code was written by the assistant under my direction.

My part:
- I set the requirements and chose between the options it proposed. One example: syncing Wi-Fi only while charging was my idea.
- I approved every flash.
- I ran the hardware tests: battery runs, sleep/wake checks and visual checks.
- I observed and reported the faults the fixes are based on.
- I review the changes and work to understand the implementation.

All measurements in this repository come from tests on the physical watch.
