# wled_c — WLED core LED control rewritten as an ESP-IDF component.
#
# This directory is a standard ESP-IDF component. Reference it from any
# ESP-IDF project via:
#   set(EXTRA_COMPONENT_DIRS path/to/wlen_c)
#
# Implementation sources are registered per-module by the tasks that
# introduce them; this baseline registers the public headers only.
idf_component_register(
    SRCS "src/wled_c_version.c"
         "src/wled_c_color.c"
         "src/wled_c_palette.c"
         "src/wled_c_rmt.c"
         "src/wled_c_bus.c"
         "src/wled_c_segment.c"
         "src/wled_c_2d.c"
         "src/wled_c_transition.c"
         "src/wled_c_fx.c"
         "src/wled_c_fx_support.c"
         "src/wled_c_fx_1d.c"
         "src/wled_c_fx_2d.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES esp_timer
)

# `driver` is a PUBLIC requirement: wled_c_rmt.h includes driver/rmt_tx.h,
# so downstream components need the RMT TX types visible.