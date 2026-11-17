#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace yad {

struct ParsedUrl {
    std::string scheme;  // "http", "https", "ws", "wss"
    std::string host;
    uint16_t port = 80;
    std::string path = "/";  // includes query string
    bool isTls = false;
    bool valid = false;

    static ParsedUrl parse(const std::string &raw, bool forWebSocket = false);
};

// Pure C++ streaming HTTP/1.1 response parser (supports Content-Length, Transfer-Encoding: chunked, EOF, and optional gzip).
class HttpResponseParser {
public:
    enum class State { Headers, Body, Done, Error };

    HttpResponseParser();
    ~HttpResponseParser();
    HttpResponseParser(const HttpResponseParser &) = delete;
    HttpResponseParser &operator=(const HttpResponseParser &) = delete;

    void reset();
    State feed(const char *data, size_t len, const std::function<void(const char *, size_t)> &onBodyChunk);
    void finishEof();

    State state() const { return state_; }
    int statusCode() const { return statusCode_; }

private:
    enum class BodyMode { Unknown, ContentLength, Chunked, UntilEof };
    struct GzipCtx;

    void emitBody(const char *data, size_t len, const std::function<void(const char *, size_t)> &onBodyChunk);

    State state_ = State::Headers;
    BodyMode bodyMode_ = BodyMode::Unknown;
    int statusCode_ = 0;
    size_t contentLength_ = 0;
    size_t bodyReceived_ = 0;
    bool gzipEncoded_ = false;
    GzipCtx *gzipCtx_ = nullptr;
    std::string buf_;

    // Chunked decoder state
    enum class ChunkState { SizeLine, Data, CrLfAfterData };
    ChunkState chunkState_ = ChunkState::SizeLine;
    size_t chunkRemaining_ = 0;
};

std::string buildHttpGetRequest(const ParsedUrl &url, bool acceptGzip = false);

// Pure C++ RFC 6455 WebSocket client framer (upgrade handshake + masked TX + RX frames).
class WebSocketFramer {
public:
    enum class State { Handshake, Open, Closed, Error };

    void reset();
    std::string buildUpgradeRequest(const ParsedUrl &url, const uint8_t random16[16]);

    // Feeds incoming bytes. Calls onText(msg) for complete text frames and appends any auto-responses (e.g. Pong) to outTx.
    void feed(const char *data,
              size_t len,
              const std::function<void(const std::string &)> &onText,
              std::string &outTx);

    std::string encodeTextFrame(const std::string &payload, const uint8_t maskKey[4]) const;
    std::string encodePongFrame(const std::string &payload, const uint8_t maskKey[4]) const;
    std::string encodePingFrame(const uint8_t maskKey[4]) const;

    State state() const { return state_; }

private:
    std::string encodeFrame(uint8_t opcode, const std::string &payload, const uint8_t maskKey[4]) const;

    State state_ = State::Handshake;
    std::string rxBuf_;
    std::string fragBuf_;
    uint8_t fragOpcode_ = 0;
};

}  // namespace yad
