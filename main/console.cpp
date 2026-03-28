#include "console.h"
#include "wifi.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "argtable3/argtable3.h"

static constexpr const char* TAG = "console";

static struct {
    struct arg_str* ssid;
    struct arg_str* password;
    struct arg_end* end;
} wifi_set_args;

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

void console_init()
{
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
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_set_cmd));

    const esp_console_cmd_t wifi_status_cmd = {
        .command = "wifi_status",
        .help = "Show WiFi connection status",
        .hint = nullptr,
        .func = &cmd_wifi_status,
        .argtable = nullptr,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_status_cmd));

    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Console ready. Type 'help' for commands.");
}
