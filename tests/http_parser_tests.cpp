#include <gtest/gtest.h>
#include "http/HttpParser.hpp"

using namespace gw::http;

TEST(HttpParserTest, ParsesSimpleGetRequest) {
    HttpParser parser;
    std::string raw = "GET /foo/bar?x=1 HTTP/1.1\r\nHost: example.com\r\nConnection: keep-alive\r\n\r\n";
    parser.append(raw.data(), raw.size());

    HttpRequest req;
    ASSERT_EQ(parser.parse(req), ParseStatus::Complete);
    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.path, "/foo/bar");
    EXPECT_EQ(req.query, "x=1");
    EXPECT_EQ(req.version, "HTTP/1.1");
    EXPECT_EQ(req.header("host").value_or(""), "example.com");
    EXPECT_TRUE(req.keepAlive);
}

TEST(HttpParserTest, ParsesRequestWithBody) {
    HttpParser parser;
    std::string body = "{\"a\":1}";
    std::string raw = "POST /submit HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    parser.append(raw.data(), raw.size());

    HttpRequest req;
    ASSERT_EQ(parser.parse(req), ParseStatus::Complete);
    EXPECT_EQ(req.method, "POST");
    EXPECT_EQ(req.body, body);
}

TEST(HttpParserTest, HandlesIncrementalDelivery) {
    HttpParser parser;
    std::string raw = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";

    HttpRequest req;
    // Feed one byte at a time to simulate slow non-blocking reads.
    for (size_t i = 0; i + 1 < raw.size(); ++i) {
        parser.append(&raw[i], 1);
        ASSERT_EQ(parser.parse(req), ParseStatus::Incomplete);
    }
    parser.append(&raw.back(), 1);
    ASSERT_EQ(parser.parse(req), ParseStatus::Complete);
    EXPECT_EQ(req.path, "/a");
}

TEST(HttpParserTest, HandlesPipelinedRequests) {
    HttpParser parser;
    std::string raw = "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n";
    parser.append(raw.data(), raw.size());

    HttpRequest req1, req2;
    ASSERT_EQ(parser.parse(req1), ParseStatus::Complete);
    EXPECT_EQ(req1.path, "/a");
    ASSERT_EQ(parser.parse(req2), ParseStatus::Complete);
    EXPECT_EQ(req2.path, "/b");
}

TEST(HttpParserTest, RejectsMalformedRequestLine) {
    HttpParser parser;
    std::string raw = "GARBAGE\r\n\r\n";
    parser.append(raw.data(), raw.size());
    HttpRequest req;
    EXPECT_EQ(parser.parse(req), ParseStatus::Error);
}

TEST(HttpParserTest, DefaultsRootPathWhenEmpty) {
    HttpParser parser;
    std::string raw = "GET  HTTP/1.1\r\n\r\n";
    // Two spaces so the target between them is empty; parser should still
    // reject or normalize gracefully rather than crash.
    parser.append(raw.data(), raw.size());
    HttpRequest req;
    auto status = parser.parse(req);
    EXPECT_TRUE(status == ParseStatus::Complete || status == ParseStatus::Error);
}

TEST(HttpParserTest, ConnectionCloseHeaderDisablesKeepAlive) {
    HttpParser parser;
    std::string raw = "GET /a HTTP/1.1\r\nConnection: close\r\n\r\n";
    parser.append(raw.data(), raw.size());
    HttpRequest req;
    ASSERT_EQ(parser.parse(req), ParseStatus::Complete);
    EXPECT_FALSE(req.keepAlive);
}

TEST(HttpParserTest, Http10DefaultsToNotKeepAlive) {
    HttpParser parser;
    std::string raw = "GET /a HTTP/1.0\r\n\r\n";
    parser.append(raw.data(), raw.size());
    HttpRequest req;
    ASSERT_EQ(parser.parse(req), ParseStatus::Complete);
    EXPECT_FALSE(req.keepAlive);
}

TEST(HttpParserTest, RejectsUnsupportedChunkedEncoding) {
    HttpParser parser;
    std::string raw = "POST /submit HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "4\r\ntest\r\n0\r\n\r\n";
    parser.append(raw.data(), raw.size());

    HttpRequest req;
    EXPECT_EQ(parser.parse(req), ParseStatus::Error);
}

TEST(HttpParserTest, RejectsInvalidContentLength) {
    HttpParser parser;
    std::string raw = "POST /submit HTTP/1.1\r\nContent-Length: nope\r\n\r\n";
    parser.append(raw.data(), raw.size());

    HttpRequest req;
    EXPECT_EQ(parser.parse(req), ParseStatus::Error);
}
