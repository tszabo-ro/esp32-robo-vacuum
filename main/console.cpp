#include "console.h"
#include "ota.h"
#include "wifi.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "argtable3/argtable3.h"

static constexpr const char* TAG = "console";

// Set by console_init; the console outlives neither app_main nor the updater.
static OtaUpdater* s_ota = nullptr;

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
    return (err == ESP_OK) ? 0 : 1;
}

static int cmd_wifi_status(int, char**)
{
    wifi_ap_record_t ap;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Connected to '%s', RSSI: %d", ap.ssid, ap.rssi);
    } else {
        ESP_LOGI(TAG, "Not connected");
    }
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

    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "neato> ";

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
        .help = "Show WiFi connection status",
        .hint = nullptr,
        .func = &cmd_wifi_status,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_status_cmd));

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

    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Console ready. Type 'help' for commands.");
}
