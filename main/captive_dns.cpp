#include "captive_dns.h"
#include "dns_message.h"

#include <cstring>
#include <lwip/sockets.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr const char* TAG = "captive_dns";

namespace {

TaskHandle_t s_task = nullptr;
int s_socket = -1;
uint32_t s_answer_addr = 0;
volatile bool s_running = false;

void dns_task(void*)
{
    uint8_t buf[dns::MAX_MESSAGE];

    while (s_running) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);

        const int received = recvfrom(s_socket, buf, sizeof(buf), 0,
                                      reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received < 0) {
            // The socket is closed from captive_dns_stop() to break this loop,
            // so an error here is the normal way out.
            if (!s_running) break;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const size_t reply_len = dns::build_reply(buf, static_cast<size_t>(received),
                                                  s_answer_addr);
        if (reply_len == 0) continue;

        sendto(s_socket, buf, reply_len, 0,
               reinterpret_cast<sockaddr*>(&from), from_len);
    }

    s_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t captive_dns_start(uint32_t ipv4_addr)
{
    if (s_running) return ESP_OK;

    s_answer_addr = ipv4_addr;

    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_socket < 0) {
        ESP_LOGE(TAG, "Could not create the DNS socket: errno %d", errno);
        return ESP_FAIL;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Could not bind port 53: errno %d", errno);
        close(s_socket);
        s_socket = -1;
        return ESP_FAIL;
    }

    s_running = true;
    if (xTaskCreate(dns_task, "captive_dns", 3072, nullptr, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the DNS task");
        s_running = false;
        close(s_socket);
        s_socket = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Captive portal DNS answering for the setup network");
    return ESP_OK;
}

void captive_dns_stop()
{
    if (!s_running) return;

    // Clearing the flag first, then closing, so the blocked recvfrom returns
    // and the task sees that it is meant to exit rather than retrying.
    s_running = false;
    if (s_socket >= 0) {
        shutdown(s_socket, SHUT_RDWR);
        close(s_socket);
        s_socket = -1;
    }
    ESP_LOGI(TAG, "Captive portal DNS stopped");
}
