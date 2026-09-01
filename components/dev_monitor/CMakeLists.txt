idf_component_register(
    SRCS "src/monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp lvgl esp_driver_usb_serial_jtag esp_lcd
)
