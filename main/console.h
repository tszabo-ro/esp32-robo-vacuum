#pragma once

class OtaUpdater;

// Start the serial console with wifi_set, wifi_status, wifi_ap and ota_update
// commands. `ota` must outlive the console.
//
// This is the USB-Serial-JTAG console only. It is not reachable over the
// network: the web interface used to forward arbitrary strings here, which made
// every command below - including the firmware updater - callable by anyone who
// could open a socket to the device.
void console_init(OtaUpdater& ota);
