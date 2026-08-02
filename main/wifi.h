#pragma once

#include "esp_err.h"

// Initialize WiFi station mode and connect using credentials from NVS.
// Returns ESP_OK once connected, ESP_ERR_NOT_FOUND if no credentials are
// stored, or ESP_FAIL/ESP_ERR_TIMEOUT if the connection did not come up in
// time.
//
// A failure return does not mean the device has stopped trying: reconnection
// continues in the background with a backoff, so an access point that comes
// back later is picked up without intervention.
esp_err_t wifi_init_sta();

// Save WiFi credentials to NVS and (re)connect.
esp_err_t wifi_set_credentials(const char* ssid, const char* password);

// Returns true if WiFi is connected and has an IP address.
bool wifi_is_connected();
