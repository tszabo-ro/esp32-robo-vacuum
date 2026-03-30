idf_component_register(
    SRCS 
    	"main.cpp" 
	"wifi.cpp" 
	"console.cpp"
	"vacuum.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        console
        esp_driver_gpio
        esp_driver_usb_serial_jtag
        esp_wifi
        nvs_flash
        mqtt
        esp_event
)

# Set C++ standard for this component
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

# Enable warnings
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
