#include "captive_dns.h"
#include "dns_message.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <lwip/sockets.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static constexpr const char* TAG = "captive_dns";

namespace {

// How long captive_dns_stop() waits for the task to actually leave its loop
// before giving up on the handshake and logging it.
constexpr uint32_t EXIT_TIMEOUT_MS = 1000;

TaskHandle_t s_task = nullptr;
int s_socket = -1;
uint32_t s_answer_addr = 0;
std::atomic<bool> s_running{false};

// Given by the task on its way out, so a stop can wait for it. Without this,
// stop() released the descriptor while the task was still inside recvfrom, and
// a quick restart could be handed the same descriptor number before the old
// task had gone - two readers on one socket, and the old one clearing s_task
// out from under the new one.
SemaphoreHandle_t s_exited = nullptr;

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

    // Published before the handshake, so a stop that is waiting on the
    // semaphore never observes a handle belonging to a task that has gone.
    s_task = nullptr;
    if (s_exited) xSemaphoreGive(s_exited);
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t captive_dns_start(uint32_t ipv4_addr)
{
    if (s_running) return ESP_OK;

    // Bound to the access point's own address rather than to everything.
    // INADDR_ANY also picks up the station interface, so with the access point
    // deliberately held up - which is what wifi_start_ap() does - this device
    // became an open resolver on the home network, answering every name with
    // the rescue address. Refusing a zero address matters for the same reason:
    // it is what bind() treats as "any".
    if (ipv4_addr == 0) {
        ESP_LOGE(TAG, "Refusing to start without an access point address");
        return ESP_ERR_INVALID_ARG;
    }
    s_answer_addr = ipv4_addr;

    if (!s_exited) {
        s_exited = xSemaphoreCreateBinary();
        if (!s_exited) ESP_LOGW(TAG, "No exit semaphore; a restart will not wait for the task");
    }
    // Clears a give left over from a previous run, so a stop cannot be
    // satisfied by the wrong task's exit.
    if (s_exited) xSemaphoreTake(s_exited, 0);

    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_socket < 0) {
        ESP_LOGE(TAG, "Could not create the DNS socket: errno %d", errno);
        return ESP_FAIL;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = ipv4_addr;

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
        s_task = nullptr;
        close(s_socket);
        s_socket = -1;
        return ESP_ERR_NO_MEM;
    }

    // Already in network byte order, so the octets read out in order.
    const auto* octets = reinterpret_cast<const uint8_t*>(&ipv4_addr);
    ESP_LOGI(TAG, "Captive portal DNS answering on %u.%u.%u.%u",
             octets[0], octets[1], octets[2], octets[3]);
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

    // Waited for rather than assumed. The descriptor has already been released
    // at this point, so returning early would let the next start() be handed it
    // while the old task is still using it.
    if (s_exited && xSemaphoreTake(s_exited, pdMS_TO_TICKS(EXIT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "DNS task did not exit within %" PRIu32 " ms", EXIT_TIMEOUT_MS);
    }

    ESP_LOGI(TAG, "Captive portal DNS stopped");
}
