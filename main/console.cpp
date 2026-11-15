#include "console.h"
#include "ota.h"
#include "wifi.h"

#include <string>
#include <cstdio>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "argtable3/argtable3.h"

static constexpr const char* TAG = "console";

// Set by console_init; the console outlives neither app_main nor the updater.
static OtaUpdater* s_ota = nullptr;

// IDF 5.3's linenoise fallback loops on EOF without blocking when USB is
// disconnected. At priority 2 it starves IDLE, which must reclaim the stacks
// of self-deleted tasks (including each 12 KiB ota_check worker).
static void run_usb_console()
{
    char line[256] = {};
    size_t used = 0;
    bool overflow = false;
    bool prompt_needed = true;
    while (true) {
        if (!usb_serial_jtag_is_connected()) {
            used = 0;
            overflow = false;
            prompt_needed = true;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (prompt_needed) {
            std::fputs("\r\nneato> ", stdout);
            std::fflush(stdout);
            prompt_needed = false;
        }
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100)) <= 0) {
            // Also block on driver errors, not just an empty RX queue.
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (!used && !overflow) continue; // Includes the LF of CRLF.
            std::fputc('\n', stdout);
            if (overflow) {
                std::puts("Command too long; discarded");
            } else {
                line[used] = '\0';
                int command_result = 0;
                const esp_err_t err = esp_console_run(line, &command_result);
                if (err == ESP_ERR_NOT_FOUND) std::puts("Unrecognized command");
                else if (err != ESP_OK) std::printf("Console error: %s\n", esp_err_to_name(err));
                else if (command_result != 0) std::printf("Command failed: %d\n", command_result);
            }
            used = 0;
            overflow = false;
            prompt_needed = true;
        } else if ((c == '\b' || c == 0x7f) && !overflow) {
            if (used) { --used; std::fputs("\b \b", stdout); }
        } else if (c >= ' ' && c < 0x7f && !overflow) {
            if (used + 1 < sizeof(line)) {
                line[used++] = c;
                std::fputc(c, stdout);
            } else overflow = true;
        }
        std::fflush(stdout);
    }
}

static struct {
    struct arg_str* ssid;
    struct arg_str* password;
    struct arg_end* end;
} wifi_set_args;

static struct {
    struct arg_str* url;
    struct arg_end* end;
} ota_update_args;

static int cmd_wifi_set(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&wifi_set_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_set_args.end, argv[0]);
        return 1;
    }

    esp_err_t err = wifi_set_credentials(wifi_set_args.ssid->sval[0],
                                         wifi_set_args.password->sval[0]);
    if (err == ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "SSID must be 1-%d characters and the password at most %d",
                 static_cast<int>(WIFI_MAX_SSID), static_cast<int>(WIFI_MAX_PASSWORD));
        return 1;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save credentials: %s", esp_err_to_name(err));
        return 1;
    }

    // Saved, not connected: the attempt runs on its own task now, so this
    // returns before there is an outcome. 'wifi_status' reports it.
    ESP_LOGI(TAG, "Credentials saved, connecting in the background");
    return 0;
}

static int cmd_wifi_status(int, char**)
{
    // Reports the address, because with the robot closed this console is the
    // one place it can be asked for. It used to print only the SSID and RSSI,
    // and to call a bare association "Connected" - erasing exactly the
    // associated-without-a-lease case the state machine and the LED exist to
    // tell apart, and leaving the address readable only in a boot log line that
    // has long since scrolled away.
    ESP_LOGI(TAG, "State: %s", wifi_state_to_string(wifi_state()));

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "Associated with '%s', RSSI: %d dBm", ap.ssid, ap.rssi);
    }

    const std::string sta = wifi_sta_address();
    if (!sta.empty()) {
        ESP_LOGI(TAG, "Address: http://%s", sta.c_str());
    } else {
        ESP_LOGI(TAG, "Address: none (no DHCP lease)");
    }

    if (wifi_ap_active()) {
        const std::string ap_addr = wifi_ap_address();
        ESP_LOGI(TAG, "Setup access point '%s' up: http://%s (%s)",
                 wifi_ap_ssid().c_str(), WIFI_SETUP_HOSTNAME,
                 ap_addr.empty() ? "no address" : ap_addr.c_str());
    }
    return 0;
}

static int cmd_wifi_ap(int, char**)
{
    esp_err_t err = wifi_start_ap();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the access point: %s", esp_err_to_name(err));
        return 1;
    }
    ESP_LOGI(TAG, "Access point '%s' requested", wifi_ap_ssid().c_str());
    return 0;
}

static int cmd_ota_update(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**)&ota_update_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, ota_update_args.end, argv[0]);
        return 1;
    }

    esp_err_t err = s_ota->start(ota_update_args.url->sval[0]);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "An update is already in progress");
        return 1;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start update: %s", esp_err_to_name(err));
        return 1;
    }

    ESP_LOGI(TAG, "Update started, watch the log for progress");
    return 0;
}

void console_init(OtaUpdater& ota)
{
    s_ota = &ota;

    esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
    config.max_cmdline_length = 256;
    ESP_ERROR_CHECK(esp_console_init(&config));
    ESP_ERROR_CHECK(esp_console_register_help_command());

    wifi_set_args.ssid = arg_str1(nullptr, nullptr, "<ssid>", "WiFi SSID");
    wifi_set_args.password = arg_str1(nullptr, nullptr, "<password>", "WiFi password");
    wifi_set_args.end = arg_end(2);

    const esp_console_cmd_t wifi_set_cmd = {
        .command = "wifi_set",
        .help = "Set WiFi credentials and connect",
        .hint = "<ssid> <password>",
        .func = &cmd_wifi_set,
        .argtable = &wifi_set_args,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_set_cmd));

    const esp_console_cmd_t wifi_status_cmd = {
        .command = "wifi_status",
        .help = "Show network state, signal and the device's address",
        .hint = nullptr,
        .func = &cmd_wifi_status,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_status_cmd));

    const esp_console_cmd_t wifi_ap_cmd = {
        .command = "wifi_ap",
        .help = "Bring up the fallback access point and keep it up",
        .hint = nullptr,
        .func = &cmd_wifi_ap,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_ap_cmd));

    ota_update_args.url = arg_str1(nullptr, nullptr, "<url>", "HTTPS URL of the firmware image");
    ota_update_args.end = arg_end(1);

    const esp_console_cmd_t ota_update_cmd = {
        .command = "ota_update",
        .help = "Download and install firmware over HTTPS, then reboot",
        .hint = "<url>",
        .func = &cmd_ota_update,
        .argtable = &ota_update_args,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ota_update_cmd));

    usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_vfs_use_driver();
    ESP_LOGI(TAG, "Console ready. Type 'help' for commands.");
    run_usb_console();
}
