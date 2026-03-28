#pragma once

#include "esp_err.h"

// Initialize WiFi station mode and connect using credentials from NVS.
// Returns ESP_OK if credentials were found and connection was initiated.
// Returns ESP_ERR_NOT_FOUND if no credentials are stored.
esp_err_t wifi_init_sta();

// Save WiFi credentials to NVS and (re)connect.
esp_err_t wifi_set_credentials(const char* ssid, const char* password);

// Returns true if WiFi is connected and has an IP address.
bool wifi_is_connected();
