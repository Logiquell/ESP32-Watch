# Firmware architecture

ESP-IDF v5.4 application, single component (`main/`), about 1,800 lines of C. One watch face is compiled in, chosen in menuconfig.

## Source files

| File | Responsibility |
|---|---|
| `main.c` | Boot sequence; LVGL clock setup; `face_tick` and `battery_tick` timers; low-battery shutdown; diagnostics (reset reason, restart counter, drain tracking, PM stats, snapshot in RTC memory) |
| `screen.c/.h` | Screen sleep/wake state machine; brightness fade; AMOLED SLEEP IN/OUT; power-management configuration and lock; LVGL timer slow-down while dark; GPIO sleep exemptions |
| `battery.c/.h` | AXP2101 driver: init (enable gauge + ADC), `battery_read()`, `battery_is_critical()`, `battery_power_off()` |
| `rtc.c/.h` | PCF85063 driver: `ext_rtc_init/read/write`, UTC storage, oscillator-stop check |
| `wifi_time.c/.h` | Wi-Fi + SNTP sync task, sync policy (charge-only), statistics |
| `face.h` | Interface every watch face implements |
| `face_flip.c` | Split-flap face (default) |
| `face_lcd.c` | Seven-segment LCD face |
| `Kconfig.projbuild` | menuconfig "Watch" menu |
| `wifi_secrets.example.h` | Template for the git-ignored `wifi_secrets.h` |

Project-level configuration:
- `sdkconfig.defaults`: flash/PSRAM, CPU, power management, core dump, LVGL options.
- `partitions.csv`: nvs, phy_init, 8 MB app, 64 KB core dump, 7 MB storage (unused so far).
- `dependencies.lock`: pinned component versions.

## Boot sequence (`app_main`)

1. **Reset diagnostics.**
   - Read the reset reason.
   - Update the restart counter (`RTC_NOINIT_ATTR`, survives resets but not a power-on).
   - Print the diagnostics snapshot saved before the last reset.
2. **Battery check before the display starts.** `battery_init()`, then `battery_read()`. If the battery is critical, power off immediately. This prevents a brownout loop.
3. **Time.**
   - Set `TZ`.
   - `seed_time()` reads the RTC. If the RTC is invalid, it falls back to the compile timestamp.
   - The result (`time_known`) is passed to the Wi-Fi module.
4. **Display.**
   - `bsp_display_start()` initialises QSPI, the panel, touch, I²C and the LVGL task.
   - `use_tickless_lvgl_clock()` replaces the port's 5 ms tick timer with an `esp_timer`-based clock.
5. **UI, under the display lock.**
   - `face_create()`
   - the first `battery_tick`/`face_tick`
   - LVGL timers: `face_tick` every 250 ms, `battery_tick` every 5 s
   - `screen_init()`
6. **`wifi_time_start(time_known)`**, which starts the sync task.

## Runtime structure

Execution contexts:
- **LVGL task** (from `esp_lvgl_port`): runs all LVGL timers. All drawing and all face, screen and battery logic happen here, so they are serialised by the display lock.
  - `face_tick` (250 ms):
    - Reads `gettimeofday`.
    - Calls `face_set_time`/`face_set_sync`/`face_set_battery` with a shared 1 Hz blink phase.
    - Skipped while the screen is dark.
  - `battery_tick` (5 s, also while dark):
    - Reads the PMU and caches the result for the face.
    - Tracks the unplugged run and saves the diagnostics snapshot.
    - Applies the critical-battery rule (two readings in a row).
    - Every 12th call (60 s) it logs diagnostics, including `esp_pm_dump_locks`.
  - `screen.c` `poll` (100 ms): edge-detects the BOOT button and checks `lv_display_get_inactive_time()` for timeout and touch-wake.
- **`wifi_time` task** (priority 3): once a minute, decides whether a sync is due, then runs `sync_once()` (blocking, up to ~45 s).
- **Wi-Fi/LwIP/event tasks** (ESP-IDF).

Data flow:

```
AXP2101 ──I²C──► battery_read() ──► s_battery (cache) ──► face_set_battery()
                         │                └─► low-battery rule ──► face_show_low_battery() / battery_power_off()
                         └─► usb_power ──► wifi_time sync policy
PCF85063 ──I²C──► seed_time() ──► system clock ◄── SNTP (Wi-Fi) ──► ext_rtc_write()
system clock ──► face_tick() ──► face_set_time()
touch / BOOT ──► LVGL inactivity time / GPIO0 ──► screen.c poll ──► sleep / wake
```

## Screen and power state machine (`screen.c`)

| State | PM lock | LVGL timers | Panel | CPU |
|---|---|---|---|---|
| **Awake** | `NO_LIGHT_SLEEP` held | refresh 15 ms, touch read 15 ms | on, brightness 80 % | DFS 40–240 MHz |
| **Fading out** (300 ms) | held | unchanged | brightness ramps to 0 | DFS |
| **Dark** | released | refresh 1 s, touch read 100 ms | DISPLAY OFF + SLEEP IN | 40 MHz + automatic light sleep |
| **Waking** | acquire lock | restore periods | SLEEP OUT, 120 ms, DISPLAY ON | — |

- **Entering dark:** timeout without touch, or a BOOT press.
- **Waking:** a touch (inactivity time < 200 ms) or a BOOT press. On wake:
  1. `on_screen_wake()` in `main.c` refreshes battery and time, with no flip animation.
  2. The screen is invalidated.
  3. Brightness fades in over 120 ms.
- **USB connected:** `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` keeps the chip out of light sleep, so logging and flashing keep working.

### LVGL clock

`esp_lvgl_port` normally advances LVGL's tick from a periodic 5 ms `esp_timer`. That wakes the CPU 200 times a second and all but prevents light sleep. `main.c` instead:
1. Sets `lv_tick_set_cb()` to read `esp_timer_get_time()`, which stays correct across light sleep.
2. Calls `lvgl_port_stop()` to stop the tick timer.
3. Re-enables LVGL's timers with `lv_timer_enable(true)`, because `lvgl_port_stop()` also pauses them.

### Panel handle access

The BSP doesn't expose the `esp_lcd` panel handles. `screen.c` reads them from the start of the LVGL display's driver data, mirroring the leading fields of `lvgl_port_display_ctx_t` in esp_lvgl_port 2.x. It sanity-checks the fields and disables panel sleep if they look wrong. This depends on esp_lvgl_port internals; see Known limitations in [development.md](development.md).

## Wi-Fi time sync policy (`wifi_time.c`)

The task checks every 60 s:

| Power source | Sync when |
|---|---|
| USB | once after plug-in (failed attempts retried every 5 min), then every `CONFIG_WATCH_RESYNC_HOURS` (default 6) |
| Battery | only if the RTC had no valid time, every 30 min until it succeeds |

`sync_once()`:
1. Starts Wi-Fi and waits up to 30 s for an IP address.
2. Starts SNTP and waits up to 15 s.
3. Stops Wi-Fi.

On success, the time is written to the RTC. The module counts attempts, failures, radio-on time and the last disconnect reason. All long delays use a 64-bit ms-to-tick conversion; see [development.md](development.md).

## Diagnostics

| What | Where | Survives |
|---|---|---|
| Reset reason, restart counter | `RTC_NOINIT` variables, logged every 60 s | resets, not power-on |
| Battery run (start %, last %, minutes), Wi-Fi stats, `esp_pm_dump_locks()` text (1,200 B) | `RTC_NOINIT saved_diag_t`, updated every 5 s, PM text every 60 s, printed once at next boot | resets, not power-on |
| Crash dump (ELF) | `coredump` flash partition | everything; read with `idf.py coredump-info` |

USB-Serial-JTAG stops responding after light sleep. The workflow after a test on battery is therefore:
1. Plug in USB.
2. Reset the watch.
3. Read the boot log.

## Watch faces

The faces implement `face.h`. Every update compares against the previous value and only touches LVGL objects that changed, so a steady face triggers no redraws.

- **`face_flip.c`**:
  - Two split-flap cards (HH, MM). Each card has separate top and bottom halves.
  - On change, the old top flap collapses (`transform_scale_y`) and the new bottom flap swings down, 170 ms per half.
  - Numerals are rendered with Tiny TTF from the embedded Ubuntu Medium font, at 160 px.
  - The glow is 8 offset, low-opacity copies drawn in a draw-event callback. LVGL's blur would band across the 20-line draw strips.
  - Shows the date at the top and the battery percentage at the bottom. The battery figure blinks at ≤ 15 %.
  - Has no Wi-Fi status indicator.
- **`face_lcd.c`**:
  - Seven-segment digits drawn as rectangles, with faint "ghost" segments.
  - Shows HH:MM with a blinking colon, plus seconds, the date and weekday flags.
  - Indicators: 4-bar battery gauge, CHG, SYNC, ERR.
