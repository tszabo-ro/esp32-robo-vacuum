#pragma once

#include <cstdint>
#include "esp_err.h"

// Answers every DNS query with the device's own address, for as long as the
// rescue access point is up.
//
// This is what makes the access point a usable recovery path rather than a
// technically-correct one. A phone or laptop that joins a network probes a
// known URL to decide whether it has working internet; with nothing answering
// DNS, the probe fails, the client reports "no internet", and both macOS and
// Android will quietly wander back to a network that has some - which is
// exactly what happens when you are standing next to a robot trying to
// re-provision it.
//
// Pointing every name at the device turns that probe into a hit on the web
// interface, which the client then shows in its captive-portal window. Joining
// the network opens the setup page by itself, and the client stays put.
//
// Only run while the access point is up: outside that, hijacking every name
// the device is asked about would be wrong rather than helpful.
esp_err_t captive_dns_start(uint32_t ipv4_addr);
void captive_dns_stop();
