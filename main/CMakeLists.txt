idf_component_register(
    SRCS "main.c"
         "pet_core.c"
         "pet_save.c"
         "pet_ui.c"
         "font_game_22.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl nvs_flash dev_monitor
)
