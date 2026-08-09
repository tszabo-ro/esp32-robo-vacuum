#pragma once

#include <cstddef>
#include <cstdint>

// The DNS message handling behind the captive portal responder, separated from
// the socket that feeds it.
//
// This parses packets chosen by whoever is in radio range, which is reason
// enough for it to be reachable from a host test rather than only from a real
// client on a real network. It has no ESP-IDF dependency; see test/.
namespace dns {

// A DNS message over UDP without EDNS cannot exceed this.
inline constexpr size_t MAX_MESSAGE = 512;

// Rewrites `msg` in place into a reply and returns the reply's length, or 0 if
// the query is not one to answer at all.
//
// `len` is how many bytes were received; `msg` must point at a buffer of at
// least MAX_MESSAGE bytes, because the answer is appended after the question.
// `answer_addr` is an IPv4 address already in network byte order.
//
// A query for anything but an internet A record gets an empty NOERROR rather
// than an address of the wrong type: a client asking for AAAA must not be
// handed four bytes and told they are an IPv6 address.
size_t build_reply(uint8_t* msg, size_t len, uint32_t answer_addr);

}  // namespace dns
