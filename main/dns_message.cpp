#include "dns_message.h"

#include <cstring>

namespace dns {
namespace {

constexpr size_t HEADER_BYTES = 12;
constexpr uint16_t TYPE_A = 1;
constexpr uint16_t CLASS_IN = 1;
constexpr uint32_t ANSWER_TTL_SECONDS = 60;

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
        // Guards against an offset that wraps past the end of the buffer on a
        // long chain of labels rather than landing inside it.
        if (label > len - offset) return 0;
        offset += 1 + label;
    }
    return 0;
}

}  // namespace

size_t build_reply(uint8_t* msg, size_t len, uint32_t answer_addr)
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
    // answer_addr is already in network byte order.
    memcpy(answer + 12, &answer_addr, 4);

    write_u16(msg + 6, 1); // ANCOUNT
    return question_end + ANSWER_BYTES;
}

}  // namespace dns
