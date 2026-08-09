// Host tests for the firmware's parsers and renderers.
//
// No test framework on purpose: the whole value of these is that they run
// anywhere a C++ compiler exists, with nothing to install first. A test that
// needs setting up is a test that stops being run.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "dns_message.h"
#include "text_util.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void report(bool ok, const char* expr, int line)
{
    g_checks++;
    if (ok) return;
    g_failures++;
    std::printf("  FAIL line %d: %s\n", line, expr);
}

void report_str(const std::string& got, const std::string& want, const char* expr, int line)
{
    g_checks++;
    if (got == want) return;
    g_failures++;
    std::printf("  FAIL line %d: %s\n    got  \"%s\"\n    want \"%s\"\n",
                line, expr, got.c_str(), want.c_str());
}

#define CHECK(cond) report((cond), #cond, __LINE__)
#define CHECK_STR(got, want) report_str((got), (want), #got, __LINE__)

// --- text::redact_uri ---

void test_redact_uri()
{
    std::printf("text::redact_uri\n");

    // Nothing to hide comes back untouched.
    CHECK_STR(text::redact_uri("mqtt://broker.local:1883"), "mqtt://broker.local:1883");
    CHECK_STR(text::redact_uri(""), "");
    CHECK_STR(text::redact_uri("not-a-uri"), "not-a-uri");

    CHECK_STR(text::redact_uri("mqtt://user:secret@broker.local:1883"),
              "mqtt://***@broker.local:1883");
    CHECK_STR(text::redact_uri("mqtts://user@broker.local"), "mqtts://***@broker.local");

    // An '@' in the path is not userinfo, and redacting to it would throw away
    // the host as well as leaving nothing actually hidden.
    CHECK_STR(text::redact_uri("ws://broker.local/path@thing"), "ws://broker.local/path@thing");

    // The password is what must not survive, whatever it contains.
    CHECK(text::redact_uri("mqtt://u:p@ss@host").find("p@ss") == std::string::npos);
    CHECK(text::redact_uri("mqtt://user:secret@host").find("secret") == std::string::npos);
}

// --- text::escape_serial ---

void test_escape_serial()
{
    std::printf("text::escape_serial\n");

    CHECK_STR(text::escape_serial("hello", 5), "hello");
    CHECK_STR(text::escape_serial("", 0), "");

    // Newline and tab survive; a carriage return is dropped.
    CHECK_STR(text::escape_serial("a\nb\tc", 5), "a\nb\tc");
    CHECK_STR(text::escape_serial("a\r\nb", 4), "a\nb");

    // A NUL must not truncate anything, which is the reason this exists.
    CHECK_STR(text::escape_serial("a\0b", 3), "a\\x00b");

    // High bytes are not valid UTF-8 and would corrupt the JSON frame.
    CHECK_STR(text::escape_serial("\xff", 1), "\\xff");
    CHECK_STR(text::escape_serial("\x1b[0m", 4), "\\x1b[0m");
    CHECK_STR(text::escape_serial("\x7f", 1), "\\x7f");

    // The printable range's own boundaries.
    CHECK_STR(text::escape_serial(" ", 1), " ");
    CHECK_STR(text::escape_serial("~", 1), "~");

    // Whatever comes out has to be something a JSON string can carry: only the
    // backslashes this function itself introduced, and no control bytes.
    const char raw[] = {'\x01', 'a', '\x02', '\xfe'};
    const std::string out = text::escape_serial(raw, sizeof(raw));
    for (unsigned char c : out) {
        CHECK(c == '\\' || (c >= 0x20 && c < 0x7f));
    }
}

// --- text::origin_matches_host ---

void test_origin_matches_host()
{
    std::printf("text::origin_matches_host\n");

    CHECK(text::origin_matches_host("http://neato.local", "neato.local"));
    CHECK(text::origin_matches_host("http://192.168.255.1", "192.168.255.1"));
    CHECK(text::origin_matches_host("http://neato.local:8080", "neato.local:8080"));

    // The cases this check exists for.
    CHECK(!text::origin_matches_host("http://evil.example", "neato.local"));
    CHECK(!text::origin_matches_host("https://neato.local", "neato.local"));
    CHECK(!text::origin_matches_host("null", "neato.local"));
    CHECK(!text::origin_matches_host("", "neato.local"));
    CHECK(!text::origin_matches_host("http://", "neato.local"));

    // A prefix match is not a match: these are different hosts.
    CHECK(!text::origin_matches_host("http://neato.local.evil.example", "neato.local"));
    CHECK(!text::origin_matches_host("http://neato.loca", "neato.local"));

    // A port is part of the origin, so it has to be part of the comparison.
    CHECK(!text::origin_matches_host("http://neato.local:8080", "neato.local"));
    CHECK(!text::origin_matches_host("http://neato.local", "neato.local:8080"));

    // An empty Host cannot authorize anything.
    CHECK(!text::origin_matches_host("http://neato.local", ""));
}

// --- text::is_json_content_type ---

void test_is_json_content_type()
{
    std::printf("text::is_json_content_type\n");

    CHECK(text::is_json_content_type("application/json"));
    CHECK(text::is_json_content_type("application/json;charset=utf-8"));
    CHECK(text::is_json_content_type("application/json; charset=utf-8"));

    // Anything that would let a cross-origin POST through without a preflight.
    CHECK(!text::is_json_content_type("text/plain"));
    CHECK(!text::is_json_content_type("application/x-www-form-urlencoded"));
    CHECK(!text::is_json_content_type("multipart/form-data"));
    CHECK(!text::is_json_content_type(""));

    // A longer type that merely starts with the right characters is not it.
    CHECK(!text::is_json_content_type("application/jsonp"));
    CHECK(!text::is_json_content_type("application/json-seq"));
}

// --- text::constant_time_equal ---

void test_constant_time_equal()
{
    std::printf("text::constant_time_equal\n");

    const uint8_t a[] = {1, 2, 3, 4};
    const uint8_t b[] = {1, 2, 3, 4};
    const uint8_t first_differs[] = {9, 2, 3, 4};
    const uint8_t last_differs[] = {1, 2, 3, 9};

    CHECK(text::constant_time_equal(a, b, 4));
    CHECK(!text::constant_time_equal(a, first_differs, 4));
    CHECK(!text::constant_time_equal(a, last_differs, 4));

    // A zero-length comparison is vacuously equal, and must not read anything.
    CHECK(text::constant_time_equal(a, first_differs, 0));

    // A difference past the compared length is not a difference.
    CHECK(text::constant_time_equal(a, last_differs, 3));
}

// --- dns::build_reply ---

// Builds a well-formed A-record query for `name`, e.g. "example.com".
std::vector<uint8_t> make_query(const std::string& name,
                                uint16_t qtype = 1, uint16_t qclass = 1,
                                uint16_t flags = 0x0100, uint16_t qdcount = 1)
{
    std::vector<uint8_t> q;
    auto push16 = [&q](uint16_t v) {
        q.push_back(static_cast<uint8_t>(v >> 8));
        q.push_back(static_cast<uint8_t>(v & 0xff));
    };

    push16(0x1234); // ID
    push16(flags);
    push16(qdcount);
    push16(0); // ANCOUNT
    push16(0); // NSCOUNT
    push16(0); // ARCOUNT

    size_t start = 0;
    while (start <= name.size()) {
        const size_t dot = name.find('.', start);
        const size_t end = (dot == std::string::npos) ? name.size() : dot;
        q.push_back(static_cast<uint8_t>(end - start));
        for (size_t i = start; i < end; i++) q.push_back(static_cast<uint8_t>(name[i]));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    q.push_back(0); // root label

    push16(qtype);
    push16(qclass);
    return q;
}

uint16_t read16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// A buffer of the size build_reply is documented to require, so the answer has
// somewhere to go.
std::vector<uint8_t> into_buffer(const std::vector<uint8_t>& query)
{
    std::vector<uint8_t> buf(dns::MAX_MESSAGE, 0);
    std::memcpy(buf.data(), query.data(), query.size());
    return buf;
}

void test_dns_valid_query()
{
    std::printf("dns::build_reply - well-formed queries\n");

    const auto query = make_query("example.com");
    auto buf = into_buffer(query);
    const uint32_t addr = 0x01ffa8c0; // 192.168.255.1, network byte order

    const size_t len = dns::build_reply(buf.data(), query.size(), addr);

    // Question echoed back, plus a 16-byte answer.
    CHECK(len == query.size() + 16);

    // The ID must survive, or the client discards the reply.
    CHECK(read16(buf.data()) == 0x1234);

    // QR=1, AA=1, RD copied back from the query.
    CHECK(read16(buf.data() + 2) == 0x8500);
    CHECK(read16(buf.data() + 4) == 1); // QDCOUNT
    CHECK(read16(buf.data() + 6) == 1); // ANCOUNT
    CHECK(read16(buf.data() + 8) == 0); // NSCOUNT
    CHECK(read16(buf.data() + 10) == 0); // ARCOUNT

    const uint8_t* answer = buf.data() + query.size();
    CHECK(read16(answer) == 0xc00c);     // pointer back to the question
    CHECK(read16(answer + 2) == 1);      // TYPE A
    CHECK(read16(answer + 4) == 1);      // CLASS IN
    CHECK(read16(answer + 10) == 4);     // RDLENGTH
    CHECK(std::memcmp(answer + 12, &addr, 4) == 0);

    // A query with RD clear must not come back with it set.
    auto no_rd = make_query("example.com", 1, 1, 0x0000);
    auto buf2 = into_buffer(no_rd);
    dns::build_reply(buf2.data(), no_rd.size(), addr);
    CHECK(read16(buf2.data() + 2) == 0x8400);
}

void test_dns_wrong_question_type()
{
    std::printf("dns::build_reply - questions that get no address\n");

    const uint32_t addr = 0x01ffa8c0;

    // AAAA must not be handed four bytes and told they are an IPv6 address.
    const auto aaaa = make_query("example.com", 28, 1);
    auto buf = into_buffer(aaaa);
    const size_t len = dns::build_reply(buf.data(), aaaa.size(), addr);
    CHECK(len == aaaa.size());           // question echoed, nothing appended
    CHECK(read16(buf.data() + 6) == 0);  // ANCOUNT stays zero

    // A class other than IN, likewise.
    const auto chaos = make_query("example.com", 1, 3);
    auto buf2 = into_buffer(chaos);
    const size_t len2 = dns::build_reply(buf2.data(), chaos.size(), addr);
    CHECK(len2 == chaos.size());
    CHECK(read16(buf2.data() + 6) == 0);
}

void test_dns_malformed()
{
    std::printf("dns::build_reply - malformed and hostile input\n");

    const uint32_t addr = 0x01ffa8c0;
    std::vector<uint8_t> buf(dns::MAX_MESSAGE, 0);

    // Shorter than a header.
    CHECK(dns::build_reply(buf.data(), 0, addr) == 0);
    CHECK(dns::build_reply(buf.data(), 11, addr) == 0);

    // Longer than a UDP DNS message can be.
    CHECK(dns::build_reply(buf.data(), dns::MAX_MESSAGE + 1, addr) == 0);

    // A response reflected back at us, which must not be answered - otherwise
    // two of these point at each other and never stop.
    const auto response = make_query("example.com", 1, 1, 0x8180);
    auto rbuf = into_buffer(response);
    CHECK(dns::build_reply(rbuf.data(), response.size(), addr) == 0);

    // An opcode other than QUERY.
    const auto notify = make_query("example.com", 1, 1, 0x2100);
    auto nbuf = into_buffer(notify);
    CHECK(dns::build_reply(nbuf.data(), notify.size(), addr) == 0);

    // Not exactly one question.
    const auto none = make_query("example.com", 1, 1, 0x0100, 0);
    auto zbuf = into_buffer(none);
    CHECK(dns::build_reply(zbuf.data(), none.size(), addr) == 0);
    const auto two = make_query("example.com", 1, 1, 0x0100, 2);
    auto tbuf = into_buffer(two);
    CHECK(dns::build_reply(tbuf.data(), two.size(), addr) == 0);

    // A compression pointer in the question. There is no prior name to point
    // at, so following one would be reading whatever happens to be there.
    //
    // The message is deliberately long enough that the top two bits are the
    // only thing that can reject it: a pointer byte is 0xc0 or above, so in a
    // short message the length check catches it first and this stops testing
    // what it says it does. It did exactly that until a mutation showed the
    // pointer check could be deleted with every test still passing.
    {
        std::vector<uint8_t> ptr(dns::MAX_MESSAGE, 0);
        const auto header = make_query("a");
        std::memcpy(ptr.data(), header.data(), 12);  // header only
        ptr[12] = 0xc0;  // pointer, and 0xc0 == 192 < the 238 bytes remaining
        ptr[13] = 0x0c;
        CHECK(dns::build_reply(ptr.data(), 250, addr) == 0);

        // The other two reserved label types, which are equally not a length.
        ptr[12] = 0x80;
        CHECK(dns::build_reply(ptr.data(), 250, addr) == 0);
        ptr[12] = 0x40;
        CHECK(dns::build_reply(ptr.data(), 250, addr) == 0);
    }

    // A label that claims to run past the end of the message.
    auto over = into_buffer(make_query("example.com"));
    over[12] = 200;
    CHECK(dns::build_reply(over.data(), 29, addr) == 0);

    // A name with no root label, running off the end of what was received.
    auto unterminated = into_buffer(make_query("example.com"));
    CHECK(dns::build_reply(unterminated.data(), 14, addr) == 0);

    // A question whose QTYPE/QCLASS are cut off by the end of the message.
    const auto query = make_query("example.com");
    auto truncated = into_buffer(query);
    CHECK(dns::build_reply(truncated.data(), query.size() - 2, addr) == 0);
}

void test_dns_no_room_for_answer()
{
    std::printf("dns::build_reply - a question that leaves no room\n");

    const uint32_t addr = 0x01ffa8c0;

    // A name long enough that the question ends within 16 bytes of the buffer.
    // The answer has to be left off rather than written past the end.
    std::string name;
    for (int i = 0; i < 7; i++) {
        if (i) name += ".";
        name += std::string(63, 'a');
    }
    name += "." + std::string(31, 'b');

    const auto query = make_query(name);
    CHECK(query.size() > dns::MAX_MESSAGE - 16);
    CHECK(query.size() <= dns::MAX_MESSAGE);

    auto buf = into_buffer(query);
    // A canary just past the message: nothing may be written there.
    const size_t len = dns::build_reply(buf.data(), query.size(), addr);

    CHECK(len == query.size());          // question only
    CHECK(read16(buf.data() + 6) == 0);  // and no answer claimed
}

}  // namespace

int main()
{
    test_redact_uri();
    test_escape_serial();
    test_origin_matches_host();
    test_is_json_content_type();
    test_constant_time_equal();
    test_dns_valid_query();
    test_dns_wrong_question_type();
    test_dns_malformed();
    test_dns_no_room_for_answer();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
