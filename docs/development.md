# Development log, decisions and limitations

This summarises the Git history (16 commits, 17–19 Nov 2026) and the tests recorded in it. Commit hashes are given so each claim can be checked against the code and the commit message.

## Timeline

| Date | Commit | Change | Hardware verification recorded |
|---|---|---|---|
| 17 Nov | 0a48b65 | ESP-IDF + LVGL project on the Waveshare BSP, first clock face | — |
| 17 Nov | 6db3385 | `env.bat` for cmd; PowerShell execution-policy workaround | boots and runs on COM5 |
| 17 Nov | 16f62c7 | Wi-Fi + SNTP sync task, radio off between syncs | time synced ~3 s after boot |
| 17 Nov | 032a143 | AXP2101 battery driver | USB charging, 4192 mV, 100 % |
| 17 Nov | 6fb8c41 | Screen sleep/wake; first attempt at the SPI flush errors | 5 sleep/wake cycles; fix later found ineffective |
| 17 Nov | 3bbb1da | PCF85063 RTC driver; second SPI fix attempt | RTC within 1 s of NTP after reboot and ~90 s power-off |
| 17 Nov | 5c8b10f | Revert buffer swap (stale pixels); 20-line BSP draw buffer | no leftover pixels, no SPI errors across 5 wakes |
| 17 Nov | 2790b84, 6cf145d, e30a7a6 | Seven-segment LCD face, colour and legibility revisions | revised after viewing on the watch (not recorded in commit messages) |
| 18 Nov | 836a832 | Light sleep while dark; low-battery shutdown | 6 sleep/wake cycles on USB; light sleep not yet tested on battery |
| 18 Nov | 83f89ae | Fix false wake in light sleep; restart counter; core dump | sleep/wake by timeout, tap, BOOT on battery; 0 restarts over ~2 h |
| 18 Nov | 4b1867d | Split-flap face (now default) | boots clean, animation checked |
| 19 Nov | 9a10a08 | Fix 2-minute Wi-Fi resync; remove LVGL 5 ms tick; battery diagnostics | light sleep 88 %, ~8 %/h |
| 19 Nov | 1f6996b | Panel SLEEP IN; DFS-only CPU while screen on; charge-only Wi-Fi | 100 → 87 % in 236 min, light sleep 97 % |

## Troubleshooting case studies

### 1. Intermittent SPI flush failures (three attempts)

- **Symptom:** after waking, or when the battery label changed, parts of the screen weren't redrawn. The log showed `spi transmit (queue) color failed`.
- **Attempt 1 (6fb8c41):** move the LVGL draw buffer to internal DMA RAM using `bsp_display_start_with_config()` buffer flags. It seemed to work at first. It was later found to have no effect, because the BSP ignores those flags.
- **Analysis (3bbb1da):** the BSP allocates a 410×100 draw buffer (82 KB) in PSRAM. The SPI driver can't DMA from PSRAM, so each flush allocates an internal bounce buffer of the same size. The largest free internal DMA block measured 81,920 bytes, just under what a full-height flush needs.
- **Attempt 2 (3bbb1da):** replace the buffer after start with a 40-line internal buffer via `lv_display_set_buffers()`. That stopped the errors, but left stale pixels on screen. The root cause of the stale pixels was not found, so the change was reverted.
- **Fix (5c8b10f):** set the BSP's own `CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=20`. That makes 16 KB per flush, which always fits. Verified: no errors and no artefacts; largest free DMA block 100 KB.
- **Trade-off:** more, smaller flushes per frame, at the cost of a little throughput. The flip animation remained smooth on the watch.

### 2. Battery ran flat overnight, then a reset loop

- **Symptom (836a832):** left overnight, the watch was found flat. Each BOOT press then flashed the screen and reset.
- **Cause:** the CPU stayed at 240 MHz with the screen dark. Near empty, lighting the panel pulled the supply below the brown-out threshold.
- **Fix:**
  - ESP-IDF power management with tickless idle and automatic light sleep while dark.
  - A critical-battery rule: on battery, ≤ 3 % or < 3.35 V on two readings 5 s apart. It shows "LO" and then requests an AXP2101 soft power-off.
  - The same rule is checked at boot, before the display starts.

### 3. False wake in light sleep (ESP32-S3 errata workaround)

- **Symptom (83f89ae):** on battery, the screen faded out and woke again immediately.
- **Cause:** on the ESP32-S3, tickless idle forces `PM_SLP_DISABLE_GPIO`, which isolates all GPIOs during light sleep. Isolating the touch INT line (GPIO38) and the shared I²C bus (GPIO14/15) produced an edge that looked like a touch.
- **Fix:** exempt those pins, and the LCD/touch control lines, with `gpio_sleep_sel_dis()`. Verified: 0 restarts over ~2 h.

### 4. Battery drain 10–12 %/h, diagnosed with on-device instrumentation

- **Problem:** USB-Serial-JTAG stops working after light sleep, so nothing can be logged live during a battery test.
- **Instrumentation added (9a10a08):**
  - PM profiling: time per power mode, lock hold times, sleep counts.
  - Battery start/end % and duration per unplugged run.
  - Wi-Fi attempts, failures and radio-on time.
  - A snapshot of all this in `RTC_NOINIT` memory, printed after a USB-triggered reset.
- **Findings:**
  - `pdMS_TO_TICKS(6 h)` computes `21,600,000 × 1000` in 32 bits. The value wraps, giving a delay of ~125 s, so Wi-Fi synced every 2 minutes instead of every 6 hours. **Fix:** a 64-bit conversion (`ms_to_ticks`).
  - `esp_lvgl_port`'s 5 ms periodic tick timer woke the CPU 200 times a second. **Fix:** LVGL reads its clock from `esp_timer` through `lv_tick_set_cb()`, and the port's timer is stopped.
- **Result:** light sleep 88 % of the time, 1 Wi-Fi sync, but drain still ~8 %/h.

### 5. Remaining drain: panel, CPU lock and Wi-Fi policy

- **Analysis:** the statistics showed significant time with the screen on, during which a `CPU_FREQ_MAX` lock held 240 MHz. Brightness 0 also leaves the AMOLED controller running.
- **Changes (1f6996b):**
  - **Panel sleep:** DISPLAY OFF + SLEEP IN while dark. This needed the panel handles, which the BSP doesn't expose; see Known limitations.
  - **CPU:** the `CPU_FREQ_MAX` lock was removed, so DFS raises the frequency only while there is work.
  - **Wi-Fi only while charging:** the watch is charged daily and the RTC holds time between charges.
- **Result:** 100 % → 87 % over 236 min, untouched. That is ≈ 3.3 %/h, with light sleep 97 % of the time, 208,254 light-sleep entries and 0 rejected.
- **Follow-up (user observation, 22 Nov):** with normal wear, the watch was at 10 % at 11:00, about 2 days after being unplugged. That is ≈ 1.5–1.9 %/h, assuming it started at 100 %. So the 4-hour figure overstated the long-run drain, probably because the gauge drops faster near full charge. The exact start %, end % and duration are recorded on the device but have not been read out yet.

## Design decisions and trade-offs

| Decision | Reason | Cost / limitation |
|---|---|---|
| Light sleep, not deep sleep | wake in milliseconds, RAM and display state kept | higher sleep current than deep sleep; deep sleep would mean a full reboot (~0.5–1 s) on every wake |
| Wi-Fi sync only while charging | radio is the largest single consumer; the device is charged daily | RTC drift between charges is not corrected; drift not yet measured |
| RTC stores UTC | DST and timezone changes never need an RTC write | — |
| Own minimal AXP2101/PCF85063 drivers instead of a library | only a few registers needed; behaviour fully visible | covers only the features used |
| 20-line draw buffer | always fits the internal DMA bounce buffer | more flush overhead per frame |
| Faces chosen at compile time | simple; one face in memory | switching requires reflashing |
| Settings in menuconfig | no UI yet | not changeable on the watch |
| Flash configured as 16 MB of 32 MB | matches Waveshare's demo configuration | half the flash unused |

## Testing method

- All testing was done on the physical watch: functional checks (boot, sync, sleep/wake by timeout, touch and BOOT, visual checks of the faces), plus battery runs.
- A battery run went like this:
  1. Charge to 100 %.
  2. Unplug, and note the time and %.
  3. Leave the watch untouched, or use it normally.
  4. Plug in, reset over USB, and read the saved diagnostics from the boot log.
- There are no automated or unit tests.

## Known limitations

- **Battery figures use the AXP2101 fuel gauge:** 1 % resolution, possibly non-linear near full charge, and every run so far started at 100 %. They are single runs, not averages. No direct current measurement has been made. Battery life (about 2.5 days with normal use) is estimated from one 2-day observation, not from a full discharge.
- **USB serial stops responding after light sleep** until the chip is reset. It occasionally needs a PWR power-cycle, which clears the saved diagnostics.
- **Panel handle access is fragile:** `screen.c` reads the panel handles by mirroring the start of esp_lvgl_port 2.x's internal `lvgl_port_display_ctx_t`. A component update could change that layout. There is a sanity check that disables panel sleep if the handles look wrong.
- **The low-battery power-off path is not yet exercised** by running the battery down to the threshold.
- **Some hardware is unused:** the IMU, audio and microSD aren't used. Touch is used only for wake and activity, not for gestures.
- **The flip face has no Wi-Fi status indicator** (`face_set_sync` is a no-op).
- **The remaining standby drain is not broken down:** after the fixes, the ESP32-S3 itself sleeps 97 % of the time. The rest of the board (panel rail, touch, PSRAM, IMU, audio) hasn't been measured or switched off.
- **Comment inconsistency in `main.c`:** the comment above the restart diagnostics says they are printed every 30 s; the code logs them every 60 s.
