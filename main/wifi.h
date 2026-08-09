#pragma once

#include <string>
#include "esp_err.h"

// Longest values the WiFi driver can actually carry. Exposed so the request
// handlers can reject an over-long value where it arrives, rather than leaving
// it to be truncated on the way to the radio or refused on the way to NVS.
inline constexpr size_t WIFI_MAX_SSID = 32;
inline constexpr size_t WIFI_MAX_PASSWORD = 63;
inline constexpr size_t WIFI_MIN_AP_PASSWORD = 8;

// Initialize WiFi station mode and connect using credentials from NVS.
// Returns ESP_OK once connected, ESP_ERR_NOT_FOUND if no credentials are
// stored, or ESP_FAIL/ESP_ERR_TIMEOUT if the connection did not come up in
// time.
//
// A failure return does not mean the device has stopped trying: reconnection
// continues in the background with a backoff, so an access point that comes
// back later is picked up without intervention.
esp_err_t wifi_init_sta();

// Stores WiFi credentials and hands the connection attempt to a worker task,
// returning as soon as they are saved. Returns ESP_ERR_INVALID_ARG for a value
// the radio could not carry, or a storage error.
//
// It does not report whether the connection succeeded: joining a network takes
// tens of seconds, and this is called from the web server's single task, which
// serves nothing else while it is blocked. Watch wifi_state() for the outcome.
esp_err_t wifi_set_credentials(const char* ssid, const char* password);

// Coarse network state, in the order the status LED reports it. Exists because
// a sealed device with no network has no other way to say what is wrong: the
// difference between "cannot associate" and "associated but no DHCP lease"
// points at completely different causes.
enum class WifiState {
    Connected,      // station associated and holding an IP address
    Associating,    // credentials stored, not associated (trying, or failing)
    Associated,     // associated but no IP address yet, so DHCP is the problem
    AccessPoint,    // fallback access point serving, join it to re-provision
    NoCredentials,  // nothing stored, so the station has nothing to try
};

WifiState wifi_state();
const char* wifi_state_to_string(WifiState state);

// Signal strength of the current association in dBm, or 0 if not associated.
int wifi_rssi();

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

// The name to reach the device by while the fallback access point is up. Its
// DNS responder answers every name with the device's own address, so this
// resolves there and nothing else has to know a number.
//
// Preferred over the numeric address in anything user-facing: a browser can
// carry a cached redirect for an address it has met before, which it will act
// on without ever contacting the network, and a name it has never seen cannot.
inline constexpr const char* WIFI_SETUP_HOSTNAME = "neato.setup";

// The address the access point interface actually ended up with, as text, or
// empty if it is not up. Read from the interface rather than the configured
// constant, so it cannot disagree with reality.
std::string wifi_ap_address();

// The station's address on the home network, or empty when it has no lease.
// Non-empty is the honest test for "reachable": a station can be associated and
// still have no address, which is a different fault with a different fix.
std::string wifi_sta_address();

// Stores the fallback access point's password, applied the next time it starts.
// Must be 8 to 63 characters: WPA2's own limits, and there is no longer a path
// to an open network, because a remote request must not be able to remove the
// authentication from the interface that exists to recover the device.
esp_err_t wifi_set_ap_password(const char* password);

// True while the access point is still on the passphrase compiled into this
// firmware. That value is published in the repository and the SSID is derived
// from the MAC, so a device in this state is findable and joinable by anyone in
// range. First-run setup refuses to finish until it is changed, and the status
// endpoint reports it, because an NVS erase silently puts it back.
bool wifi_ap_password_is_default();
bool wifi_is_default_ap_password(const char* candidate);
