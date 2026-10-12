// HTTP/1.1 response parsing: launcher/net/http_parse.cpp (HTTPS client).
#include "doctest.h"
#include "http_parse.h"

#include <cstring>
#include <string>

using namespace fm2k::net;

namespace {

// Decodes `body` through a decoder for `head`, feeding `step` bytes at a time.
std::string Decode(const std::string& head_text, const std::string& body, size_t step,
                   bool* ok = nullptr, size_t* leftover = nullptr) {
    ResponseHead h;
    long n = ParseResponseHead(head_text.data(), head_text.size(), h);
    REQUIRE(n == (long)head_text.size());
    BodyDecoder d;
    REQUIRE(d.Init(h, false));
    std::string out;
    size_t pos = 0;
    bool good = true;
    while (pos < body.size() && !d.done()) {
        size_t chunk = std::min(step, body.size() - pos);
        long used = d.Feed(body.data() + pos, chunk, out);
        if (used < 0) { good = false; break; }
        pos += (size_t)used;
        if (used == 0 && !d.done()) { good = false; break; }
    }
    if (good && !d.done()) good = d.OnEof();
    if (ok) *ok = good;
    if (leftover) *leftover = body.size() - pos;
    return out;
}

}  // namespace

TEST_CASE("http: URL parsing") {
    Url u;
    REQUIRE(ParseUrl("https://hub.2dfm.org/pair/begin?x=1#frag", u));
    CHECK(u.scheme == "https");
    CHECK(u.tls);
    CHECK(u.host == "hub.2dfm.org");
    CHECK(u.port == 443);
    CHECK(u.target == "/pair/begin?x=1");
    CHECK(HostHeader(u) == "hub.2dfm.org");

    REQUIRE(ParseUrl("http://127.0.0.1:8765", u));
    CHECK_FALSE(u.tls);
    CHECK(u.port == 8765);
    CHECK(u.target == "/");
    CHECK(HostHeader(u) == "127.0.0.1:8765");

    REQUIRE(ParseUrl("wss://Example.COM:443/ws", u));
    CHECK(u.host == "example.com");
    CHECK(HostHeader(u) == "example.com");

    REQUIRE(ParseUrl("http://[::1]:8080/x", u));
    CHECK(u.host == "::1");
    CHECK(HostHeader(u) == "[::1]:8080");

    REQUIRE(ParseUrl("https://h?q", u));
    CHECK(u.target == "/?q");

    CHECK_FALSE(ParseUrl("ftp://x/", u));
    CHECK_FALSE(ParseUrl("https://user:pw@x/", u));
    CHECK_FALSE(ParseUrl("https://:443/", u));
    CHECK_FALSE(ParseUrl("https://x:0/", u));
    CHECK_FALSE(ParseUrl("https://x:70000/", u));
    CHECK_FALSE(ParseUrl("https://x:/", u));
    CHECK_FALSE(ParseUrl("no-scheme", u));
}

TEST_CASE("http: redirect resolution") {
    Url base, out;
    REQUIRE(ParseUrl("https://github.com/o/r/releases/download/v1/a.zip", base));
    REQUIRE(ResolveRedirect(base, "https://objects.githubusercontent.com/x?sig=1", out));
    CHECK(out.host == "objects.githubusercontent.com");
    CHECK(out.target == "/x?sig=1");

    REQUIRE(ResolveRedirect(base, "/login", out));
    CHECK(out.host == "github.com");
    CHECK(out.target == "/login");

    REQUIRE(ResolveRedirect(base, "b.zip", out));
    CHECK(out.target == "/o/r/releases/download/v1/b.zip");

    REQUIRE(ResolveRedirect(base, "//cdn.example/x", out));
    CHECK(out.scheme == "https");
    CHECK(out.host == "cdn.example");

    REQUIRE(ResolveRedirect(base, "?page=2", out));
    CHECK(out.target == "/o/r/releases/download/v1/a.zip?page=2");

    CHECK_FALSE(ResolveRedirect(base, "ftp://x/", out));
    CHECK_FALSE(ResolveRedirect(base, "", out));
}

TEST_CASE("http: response head parsing") {
    const std::string raw =
        "HTTP/1.1 302 Found\r\n"
        "Location: https://example.com/\r\n"
        "Content-Length:  0 \r\n"
        "X-Folded: a\r\n"
        " b\r\n"
        "\r\n"
        "BODY";
    ResponseHead h;
    // Every proper prefix needs more data.
    for (size_t i = 0; i < raw.size() - 4; ++i) {
        CHECK(ParseResponseHead(raw.data(), i, h) == 0);
    }
    long n = ParseResponseHead(raw.data(), raw.size(), h);
    CHECK(n == (long)(raw.size() - 4));
    CHECK(h.status == 302);
    CHECK(h.reason == "Found");
    REQUIRE(FindHeader(h.headers, "location"));
    CHECK(*FindHeader(h.headers, "LOCATION") == "https://example.com/");
    CHECK(*FindHeader(h.headers, "content-length") == "0");
    CHECK(*FindHeader(h.headers, "x-folded") == "a b");
    CHECK(FindHeader(h.headers, "missing") == nullptr);

    // Bare LF line endings and no reason phrase.
    const std::string lf = "HTTP/1.0 200\nA: b\n\n";
    CHECK(ParseResponseHead(lf.data(), lf.size(), h) == (long)lf.size());
    CHECK(h.status == 200);
    CHECK(h.minor_version == 0);

    // Malformed.
    for (const char* bad : {"HTTP/2 200 OK\r\n\r\n", "HTTP/1.1 2x0 OK\r\n\r\n",
                            "SSH-2.0-OpenSSH\r\n\r\n", "HTTP/1.1 200 OK\r\nNoColon\r\n\r\n",
                            "HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n"}) {
        CHECK(ParseResponseHead(bad, std::strlen(bad), h) == -1);
    }
    std::string huge = "HTTP/1.1 200 OK\r\n" + std::string(kMaxResponseHead + 10, 'a');
    CHECK(ParseResponseHead(huge.data(), huge.size(), h) == -1);
}

TEST_CASE("http: header token matching") {
    CHECK(HeaderHasToken("keep-alive, Upgrade", "upgrade"));
    CHECK(HeaderHasToken("Upgrade", "Upgrade"));
    CHECK_FALSE(HeaderHasToken("Upgraded", "upgrade"));
    CHECK(HeaderHasToken("gzip, chunked", "chunked"));
}

TEST_CASE("http: content-length body, any feed size, leftover kept") {
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n";
    for (size_t step : {size_t(1), size_t(3), size_t(100)}) {
        bool ok = false;
        size_t left = 0;
        CHECK(Decode(head, "hello worldEXTRA", step, &ok, &left) == "hello world");
        CHECK(ok);
        CHECK(left == 5);
    }
    // Truncated body is an error at EOF.
    bool ok = true;
    Decode(head, "hello", 100, &ok);
    CHECK_FALSE(ok);

    ResponseHead h;
    const std::string bad = "HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n\r\n";
    REQUIRE(ParseResponseHead(bad.data(), bad.size(), h) > 0);
    BodyDecoder d;
    CHECK_FALSE(d.Init(h, false));
}

TEST_CASE("http: chunked body with extensions and trailers") {
    const std::string head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    const std::string body =
        "4\r\nWiki\r\n"
        "5;name=val\r\npedia\r\n"
        "E\r\n in\r\n\r\nchunks.\r\n"
        "0\r\n"
        "X-Trailer: yes\r\n"
        "\r\n"
        "NEXT";
    for (size_t step : {size_t(1), size_t(2), size_t(7), size_t(1000)}) {
        bool ok = false;
        size_t left = 0;
        CHECK(Decode(head, body, step, &ok, &left) == "Wikipedia in\r\n\r\nchunks.");
        CHECK(ok);
        CHECK(left == 4);
    }
    // Upper-case hex and bare LF.
    bool ok = false;
    CHECK(Decode(head, "A\n0123456789\n0\n\n", 1, &ok) == "0123456789");
    CHECK(ok);
    // Garbage size line.
    ok = true;
    Decode(head, "zz\r\n", 10, &ok);
    CHECK_FALSE(ok);
    // Missing CRLF after data.
    ok = true;
    Decode(head, "3\r\nabcX\r\n0\r\n\r\n", 10, &ok);
    CHECK_FALSE(ok);
    // Connection closed mid-chunk.
    ok = true;
    Decode(head, "10\r\nabc", 10, &ok);
    CHECK_FALSE(ok);
}

TEST_CASE("http: until-close and bodiless responses") {
    bool ok = false;
    CHECK(Decode("HTTP/1.0 200 OK\r\n\r\n", "streamed", 3, &ok) == "streamed");
    CHECK(ok);

    for (const char* hs : {"HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n",
                           "HTTP/1.1 304 Not Modified\r\n\r\n",
                           "HTTP/1.1 101 Switching Protocols\r\n\r\n"}) {
        ResponseHead h;
        REQUIRE(ParseResponseHead(hs, std::strlen(hs), h) > 0);
        BodyDecoder d;
        REQUIRE(d.Init(h, false));
        CHECK(d.done());
    }
    ResponseHead h;
    const char* hs = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
    REQUIRE(ParseResponseHead(hs, std::strlen(hs), h) > 0);
    BodyDecoder d;
    REQUIRE(d.Init(h, /*head_request=*/true));
    CHECK(d.done());
}
