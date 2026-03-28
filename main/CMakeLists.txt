idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
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
