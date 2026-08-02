#pragma once

class OtaUpdater;

// Start the serial console with wifi_set, wifi_status and ota_update commands.
// `ota` must outlive the console.
void console_init(OtaUpdater& ota);
