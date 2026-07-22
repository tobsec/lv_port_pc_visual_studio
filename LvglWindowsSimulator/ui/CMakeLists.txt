idf_component_register(
    SRCS
        "golden_screen.c"
        "screen_manager.c"
        "map_renderer.c"
        "tile_cache.c"
        "warning_overlay.c"
        "ais_screen.c"
        "media_screen.c"
        "icons/lv_image_telltale_icons.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl__lvgl esp_lcd
    PRIV_REQUIRES settings ais_store fusion
)

# Allow #include "lvgl/lvgl.h" via build-tree forwarding header
target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_BINARY_DIR}/_lvgl_include")
