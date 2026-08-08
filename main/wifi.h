#pragma once

#include <string>
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

// Returns true if the station is connected and has an IP address.
bool wifi_is_connected();

// Returns true if the web interface is reachable on any interface: either the
// station is connected, or the fallback access point is up.
bool wifi_has_network();

// Returns true while the fallback access point is running. It is started when
// the station cannot connect, and stopped once it can.
bool wifi_ap_active();

// Brings the fallback access point up on request and keeps it up, even if the
// station reconnects. Otherwise the only way to reach it is to break the stored
// credentials, which is a poor way to exercise a recovery path.
esp_err_t wifi_start_ap();

// SSID of the fallback access point. Derived from the MAC, so it is known
// before the access point has ever started.
std::string wifi_ap_ssid();

// Stores the fallback access point's password, applied the next time it starts.
// Must be at least 8 characters for WPA2, or empty for an open network.
esp_err_t wifi_set_ap_password(const char* password);
