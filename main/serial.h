#pragma once

#include <cstdint>
#include <string_view>
#include "driver/uart.h"
#include "esp_err.h"
#include "hal/gpio_types.h"

// A hardware UART exposed as a terminal to the debug web interface.
//
// This is the link the vacuum is wired to. The USB-Serial-JTAG console is a
// separate interface and is unaffected by anything here, so opening a terminal
// in the browser cannot disturb the serial console.
class SerialPort {
public:
    // Configures the UART and installs its driver. Idempotent.
    esp_err_t start();

    // Queues bytes for transmission, returning the number accepted.
    int write(std::string_view data);

    // Blocks up to timeout_ms for incoming bytes, returning the number read.
    size_t read(char* buf, size_t len, uint32_t timeout_ms);

    // Changes the line rate, taking effect immediately. Not persisted: the
    // port returns to DEFAULT_BAUD on restart.
    esp_err_t set_baud(uint32_t baud);
    uint32_t baud() const;

    // False when the driver failed to install. read() cannot block in that
    // state, so a reader that assumes it does spins at full speed instead of
    // idling - and the status page would otherwise report a baud rate for a
    // UART that is not running.
    bool started() const;

    // UART1 driving the pins silkscreened TX/RX on the SuperMini, which are
    // UART0's defaults. They are free for this because the ESP's own console
    // runs over USB-Serial-JTAG rather than over UART0.
    static constexpr uart_port_t PORT = UART_NUM_1;
    static constexpr gpio_num_t TX_PIN = GPIO_NUM_21;
    static constexpr gpio_num_t RX_PIN = GPIO_NUM_20;
    static constexpr uint32_t DEFAULT_BAUD = 115200;

private:
    static constexpr int RX_BUFFER_BYTES = 1024;
    static constexpr int TX_BUFFER_BYTES = 1024;
    static constexpr uint32_t MIN_BAUD = 300;
    static constexpr uint32_t MAX_BAUD = 2000000;

    uint32_t baud_ = DEFAULT_BAUD;
    bool started_ = false;
};
