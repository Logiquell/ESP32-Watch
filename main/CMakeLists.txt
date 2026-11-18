idf_component_register(SRCS "main.c" "wifi_time.c" "battery.c" "screen.c" "rtc.c"
                            "face_lcd.c" "face_flip.c"
                       INCLUDE_DIRS "."
                       # Ubuntu Medium TTF (Ubuntu Font Licence) shipped with LVGL; used by face_flip.c
                       EMBED_FILES "../managed_components/lvgl__lvgl/examples/libs/tiny_ttf/Ubuntu-Medium.ttf")
