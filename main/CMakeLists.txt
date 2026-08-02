idf_component_register(
    SRCS 
    	"main.cpp" 
	"wifi.cpp" 
	"console.cpp"
	"vacuum.cpp"
	"mqtt.cpp"
	"webserver.cpp"
	"ota.cpp"
	"serial.cpp"
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
    EMBED_TXTFILES
        "index.html"
)

# Set C++ standard for this component
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

# Enable warnings
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
)
