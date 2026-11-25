idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES kvm_common bt nvs_flash esp_driver_usb_serial_jtag esp_timer esp_hw_support)
