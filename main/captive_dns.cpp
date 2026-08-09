#include "captive_dns.h"

#include <cstring>
#include <lwip/sockets.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr const char* TAG = "captive_dns";

namespace {

// A DNS message over UDP without EDNS cannot exceed this, and nothing that
// reaches a captive portal responder uses EDNS meaningfully. Anything larger is
// dropped rather than truncated.
constexpr size_t MAX_MESSAGE = 512;

constexpr size_t HEADER_BYTES = 12;
constexpr uint16_t TYPE_A = 1;
constexpr uint16_t CLASS_IN = 1;
constexpr uint32_t ANSWER_TTL_SECONDS = 60;

TaskHandle_t s_task = nullptr;
int s_socket = -1;
uint32_t s_answer_addr = 0;
volatile bool s_running = false;

uint16_t read_u16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void write_u16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xff);
}

// Returns the offset just past the QNAME, or 0 if the name is malformed or runs
// off the end of the message. Compression pointers are rejected: a query has no
// prior name to point at, so one here is either broken or hostile.
size_t skip_qname(const uint8_t* msg, size_t len, size_t offset)
{
    while (offset < len) {
        const uint8_t label = msg[offset];
        if (label == 0) return offset + 1;
        if ((label & 0xc0) != 0) return 0;
        offset += 1 + label;
    }
    return 0;
}

// Builds an answer in place, after the question, and returns the total reply
// length - or 0 if the query is not one to answer.
size_t build_reply(uint8_t* msg, size_t len)
{
    if (len < HEADER_BYTES || len > MAX_MESSAGE) return 0;

    // Not a standard query, or a response being reflected back at us.
    const uint16_t flags = read_u16(msg + 2);
    if (flags & 0x8000) return 0;              // already a response
    if (((flags >> 11) & 0x0f) != 0) return 0; // opcode other than QUERY
    if (read_u16(msg + 4) != 1) return 0;      // exactly one question

    const size_t qname_end = skip_qname(msg, len, HEADER_BYTES);
    if (qname_end == 0 || qname_end + 4 > len) return 0;

    const uint16_t qtype = read_u16(msg + qname_end);
    const uint16_t qclass = read_u16(msg + qname_end + 2);
    const size_t question_end = qname_end + 4;

    // QR=1, AA=1, and the query's own RD copied back.
    write_u16(msg + 2, static_cast<uint16_t>(0x8400 | (flags & 0x0100)));
    write_u16(msg + 6, 0);  // ANCOUNT, set below when there is one
    write_u16(msg + 8, 0);  // NSCOUNT
    write_u16(msg + 10, 0); // ARCOUNT

    // Anything that is not an internet A record gets an empty NOERROR rather
    // than an address of the wrong type. A client asking for AAAA must not be
    // handed four bytes and told they are an IPv6 address.
    if (qtype != TYPE_A || qclass != CLASS_IN) return question_end;

    constexpr size_t ANSWER_BYTES = 16;
    if (question_end + ANSWER_BYTES > MAX_MESSAGE) return question_end;

    uint8_t* answer = msg + question_end;
    write_u16(answer, 0xc000 | HEADER_BYTES); // name: pointer back to the question
    write_u16(answer + 2, TYPE_A);
    write_u16(answer + 4, CLASS_IN);
    answer[6] = static_cast<uint8_t>(ANSWER_TTL_SECONDS >> 24);
    answer[7] = static_cast<uint8_t>(ANSWER_TTL_SECONDS >> 16);
    answer[8] = static_cast<uint8_t>(ANSWER_TTL_SECONDS >> 8);
    answer[9] = static_cast<uint8_t>(ANSWER_TTL_SECONDS);
    write_u16(answer + 10, 4);
    // s_answer_addr is already in network byte order.
    memcpy(answer + 12, &s_answer_addr, 4);

    write_u16(msg + 6, 1); // ANCOUNT
    return question_end + ANSWER_BYTES;
}

void dns_task(void*)
{
    uint8_t buf[MAX_MESSAGE];

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

        const size_t reply_len = build_reply(buf, static_cast<size_t>(received));
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
