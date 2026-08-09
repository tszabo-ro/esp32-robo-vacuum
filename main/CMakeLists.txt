idf_component_register(
    SRCS
        "main.cpp"
        "wifi.cpp"
        "auth.cpp"
        "captive_dns.cpp"
        "console.cpp"
        "dns_message.cpp"
        "text_util.cpp"
        "vacuum.cpp"
        "mqtt.cpp"
        "webserver.cpp"
        "ota.cpp"
        "serial.cpp"
        "factory_reset.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        console
        esp_driver_gpio
        esp_driver_uart
        esp_driver_usb_serial_jtag
        esp_http_server
        esp_wifi
        nvs_flash
        mqtt
        esp_event
        json
        app_update
        esp_http_client
        esp_https_ota
        esp-tls
        mbedtls
        lwip
        esp_netif
    EMBED_TXTFILES
        "index.html"
)

# Set C++ standard for this component
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

# Warnings are errors here, and only here: the IDF's own components are built
# with their own settings and are not this project's to fix. CI used to grep the
# build log for "warning:" instead, which checks a string in a log rather than
# the thing itself, and passes happily if the log format ever changes.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Werror
)
