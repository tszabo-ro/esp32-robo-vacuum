#include "serial.h"

#include <cinttypes>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static constexpr const char* TAG = "serial";

esp_err_t SerialPort::start()
{
    if (started_) return ESP_OK;

    esp_err_t err = uart_driver_install(PORT, RX_BUFFER_BYTES, TX_BUFFER_BYTES, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    uart_config_t cfg = {};
    cfg.baud_rate = static_cast<int>(baud_);
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    err = uart_param_config(PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Pin assignment failed: %s", esp_err_to_name(err));
        return err;
    }

    started_ = true;
    ESP_LOGI(TAG, "UART%d ready at %" PRIu32 " baud (TX=GPIO%d, RX=GPIO%d)",
             static_cast<int>(PORT), baud_,
             static_cast<int>(TX_PIN), static_cast<int>(RX_PIN));
    return ESP_OK;
}

int SerialPort::write(std::string_view data)
{
    if (!started_ || data.empty()) return 0;
    return uart_write_bytes(PORT, data.data(), data.size());
}

size_t SerialPort::read(char* buf, size_t len, uint32_t timeout_ms)
{
    if (!started_ || len == 0) return 0;

    int received = uart_read_bytes(PORT, buf, len, pdMS_TO_TICKS(timeout_ms));
    return received > 0 ? static_cast<size_t>(received) : 0;
}

esp_err_t SerialPort::set_baud(uint32_t baud)
{
    if (baud < MIN_BAUD || baud > MAX_BAUD) return ESP_ERR_INVALID_ARG;

    if (started_) {
        esp_err_t err = uart_set_baudrate(PORT, baud);
        if (err != ESP_OK) return err;
    }

    baud_ = baud;
    // Says which of the two happened. It used to report the rate as set either
    // way, so a UART that never opened still showed a changed line rate.
    if (started_) {
        ESP_LOGI(TAG, "Baud rate set to %" PRIu32, baud);
    } else {
        ESP_LOGW(TAG, "Baud rate recorded as %" PRIu32 ", but the port is not open", baud);
    }
    return ESP_OK;
}

uint32_t SerialPort::baud() const
{
    return baud_;
}

bool SerialPort::started() const
{
    return started_;
}
