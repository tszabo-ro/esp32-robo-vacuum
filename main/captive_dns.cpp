#include "captive_dns.h"
#include "dns_message.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <new>
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

struct DnsTaskContext {
    int socket = -1;
    uint32_t answer_addr = 0;
    std::atomic<bool> running{true};
};

// The task owns its context and closes its own socket when it exits. A stop
// only shuts the socket down, so its descriptor cannot be reused underneath an
// old task that is still unwinding.
std::unique_ptr<DnsTaskContext> s_context;
SemaphoreHandle_t s_mutex = xSemaphoreCreateMutex();

// Given by the task on its way out, so a stop can wait for it. The task gives
// it before publishing that its context is gone, so a new start cannot mistake
// an old task's exit for the new task's exit.
SemaphoreHandle_t s_exited = nullptr;

class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex)
    {
        if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~Lock() { if (mutex_) xSemaphoreGive(mutex_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SemaphoreHandle_t mutex_;
};

void dns_task(void* arg)
{
    auto* context = static_cast<DnsTaskContext*>(arg);
    uint8_t buf[dns::MAX_MESSAGE];

    while (context->running) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);

        const int received = recvfrom(context->socket, buf, sizeof(buf), 0,
                                      reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received < 0) {
            // The socket is shut down from captive_dns_stop() to break this loop,
            // so an error here is the normal way out.
            if (!context->running) break;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const size_t reply_len = dns::build_reply(buf, static_cast<size_t>(received),
                                                  context->answer_addr);
        if (reply_len == 0) continue;

        sendto(context->socket, buf, reply_len, 0,
               reinterpret_cast<sockaddr*>(&from), from_len);
    }

    close(context->socket);
    if (s_exited) xSemaphoreGive(s_exited);
    {
        Lock lock(s_mutex);
        if (s_context.get() == context) s_context.reset();
    }
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t captive_dns_start(uint32_t ipv4_addr)
{
    if (!s_mutex) return ESP_ERR_NO_MEM;
    Lock lock(s_mutex);
    if (s_context) {
        // Do not start another task while the previous one is still unwinding:
        // it still owns its descriptor, even after a timed-out stop.
        return s_context->running ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

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
    if (!s_exited) {
        s_exited = xSemaphoreCreateBinary();
        if (!s_exited) ESP_LOGW(TAG, "No exit semaphore; a restart will not wait for the task");
    }
    // Clears a give left over from a previous run, so a stop cannot be
    // satisfied by the wrong task's exit.
    if (s_exited) xSemaphoreTake(s_exited, 0);

    std::unique_ptr<DnsTaskContext> context(new (std::nothrow) DnsTaskContext);
    if (!context) return ESP_ERR_NO_MEM;
    context->answer_addr = ipv4_addr;
    context->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (context->socket < 0) {
        ESP_LOGE(TAG, "Could not create the DNS socket: errno %d", errno);
        context.reset();
        return ESP_FAIL;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = ipv4_addr;

    if (bind(context->socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Could not bind port 53: errno %d", errno);
        close(context->socket);
        context.reset();
        return ESP_FAIL;
    }

    s_context = std::move(context);
    if (xTaskCreate(dns_task, "captive_dns", 3072, s_context.get(), 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the DNS task");
        close(s_context->socket);
        s_context.reset();
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
    if (!s_mutex) return;

    {
        Lock lock(s_mutex);
        if (!s_context) return;

        // Clearing the flag first, then shutting down, so the blocked recvfrom
        // returns and the task sees that it is meant to exit rather than
        // retrying. The task closes the descriptor after it has left the
        // receive loop, so it cannot be reused while that task still runs.
        s_context->running = false;
        shutdown(s_context->socket, SHUT_RDWR);
    }

    // A timeout intentionally leaves the context published. A later start then
    // refuses instead of reusing a descriptor still owned by this task.
    if (s_exited && xSemaphoreTake(s_exited, pdMS_TO_TICKS(EXIT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "DNS task did not exit within %" PRIu32 " ms", EXIT_TIMEOUT_MS);
    }

    ESP_LOGI(TAG, "Captive portal DNS stopped");
}
