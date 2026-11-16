#include "yad/net_proto.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace yad {

static std::string lowerAscii(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

ParsedUrl ParsedUrl::parse(const std::string &raw, bool forWebSocket) {
    ParsedUrl out;
    const size_t schemeEnd = raw.find("://");
    if (schemeEnd == std::string::npos) return out;

    out.scheme = lowerAscii(raw.substr(0, schemeEnd));
    if (forWebSocket) {
        if (out.scheme == "https") out.scheme = "wss";
        else if (out.scheme == "http") out.scheme = "ws";
    }

    if (out.scheme == "https" || out.scheme == "wss") {
        out.isTls = true;
        out.port = 443;
    } else if (out.scheme == "http" || out.scheme == "ws") {
        out.isTls = false;
        out.port = 80;
    } else {
        return out;
    }

    size_t hostStart = schemeEnd + 3;
    size_t pathStart = raw.find('/', hostStart);
    std::string authority;
    if (pathStart == std::string::npos) {
        authority = raw.substr(hostStart);
        out.path = "/";
    } else {
        authority = raw.substr(hostStart, pathStart - hostStart);
        out.path = raw.substr(pathStart);
        if (out.path.empty()) out.path = "/";
    }

    if (authority.empty()) return out;

    // Handle IPv6 [::1]:port or host:port
    if (authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return out;
        out.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            out.port = static_cast<uint16_t>(std::atoi(authority.c_str() + close + 2));
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            out.host = authority.substr(0, colon);
            const int p = std::atoi(authority.c_str() + colon + 1);
            if (p <= 0 || p > 65535) return out;
            out.port = static_cast<uint16_t>(p);
        } else {
            out.host = authority;
        }
    }

    out.valid = !out.host.empty();
    return out;
}

std::string buildHttpGetRequest(const ParsedUrl &url) {
    std::string req = "GET " + url.path + " HTTP/1.1\r\n";
    req += "Host: " + url.host;
    if ((url.isTls && url.port != 443) || (!url.isTls && url.port != 80)) {
        req += ":" + std::to_string(url.port);
    }
    req += "\r\nConnection: close\r\nUser-Agent: yad/1.0\r\n\r\n";
    return req;
}

void HttpResponseParser::reset() {
    state_ = State::Headers;
    bodyMode_ = BodyMode::Unknown;
    statusCode_ = 0;
    contentLength_ = 0;
    bodyReceived_ = 0;
    buf_.clear();
    chunkState_ = ChunkState::SizeLine;
    chunkRemaining_ = 0;
}

HttpResponseParser::State HttpResponseParser::feed(
    const char *data,
    size_t len,
    const std::function<void(const char *, size_t)> &onBodyChunk) {
    if (state_ == State::Done || state_ == State::Error) return state_;
    if (data && len > 0) buf_.append(data, len);

    if (state_ == State::Headers) {
        const size_t hdrEnd = buf_.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) return state_;

        const std::string hdrs = buf_.substr(0, hdrEnd);
        buf_.erase(0, hdrEnd + 4);

        size_t pos = 0;
        bool firstLine = true;
        bodyMode_ = BodyMode::UntilEof;
        while (pos < hdrs.size()) {
            size_t eol = hdrs.find("\r\n", pos);
            if (eol == std::string::npos) eol = hdrs.size();
            const std::string line = hdrs.substr(pos, eol - pos);
            pos = eol + 2;

            if (firstLine) {
                firstLine = false;
                const size_t sp = line.find(' ');
                if (sp != std::string::npos) {
                    statusCode_ = std::atoi(line.c_str() + sp + 1);
                }
                continue;
            }

            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = lowerAscii(line.substr(0, colon));
            std::string val = line.substr(colon + 1);
            size_t a = 0;
            while (a < val.size() && (val[a] == ' ' || val[a] == '\t')) ++a;
            val = lowerAscii(val.substr(a));

            if (key == "transfer-encoding" && val.find("chunked") != std::string::npos) {
                bodyMode_ = BodyMode::Chunked;
            } else if (key == "content-length" && bodyMode_ != BodyMode::Chunked) {
                bodyMode_ = BodyMode::ContentLength;
                contentLength_ = static_cast<size_t>(std::strtoull(val.c_str(), nullptr, 10));
            }
        }

        state_ = State::Body;
        if (bodyMode_ == BodyMode::ContentLength && contentLength_ == 0) {
            state_ = State::Done;
            return state_;
        }
    }

    if (state_ == State::Body) {
        if (bodyMode_ == BodyMode::UntilEof) {
            if (!buf_.empty()) {
                if (onBodyChunk) onBodyChunk(buf_.data(), buf_.size());
                buf_.clear();
            }
        } else if (bodyMode_ == BodyMode::ContentLength) {
            const size_t need = contentLength_ - bodyReceived_;
            const size_t take = std::min(need, buf_.size());
            if (take > 0) {
                if (onBodyChunk) onBodyChunk(buf_.data(), take);
                buf_.erase(0, take);
                bodyReceived_ += take;
            }
            if (bodyReceived_ >= contentLength_) {
                state_ = State::Done;
            }
        } else if (bodyMode_ == BodyMode::Chunked) {
            while (!buf_.empty() && state_ == State::Body) {
                if (chunkState_ == ChunkState::SizeLine) {
                    const size_t eol = buf_.find("\r\n");
                    if (eol == std::string::npos) break;
                    std::string line = buf_.substr(0, eol);
                    buf_.erase(0, eol + 2);
                    const size_t semi = line.find(';');
                    if (semi != std::string::npos) line.resize(semi);
                    chunkRemaining_ = static_cast<size_t>(std::strtoull(line.c_str(), nullptr, 16));
                    if (chunkRemaining_ == 0) {
                        state_ = State::Done;
                        break;
                    }
                    chunkState_ = ChunkState::Data;
                } else if (chunkState_ == ChunkState::Data) {
                    const size_t take = std::min(chunkRemaining_, buf_.size());
                    if (take == 0) break;
                    if (onBodyChunk) onBodyChunk(buf_.data(), take);
                    buf_.erase(0, take);
                    chunkRemaining_ -= take;
                    if (chunkRemaining_ == 0) {
                        chunkState_ = ChunkState::CrLfAfterData;
                    }
                } else if (chunkState_ == ChunkState::CrLfAfterData) {
                    if (buf_.size() < 2) break;
                    buf_.erase(0, 2);
                    chunkState_ = ChunkState::SizeLine;
                }
            }
        }
    }
    return state_;
}

void HttpResponseParser::finishEof() {
    if (state_ == State::Body && bodyMode_ == BodyMode::UntilEof) {
        state_ = State::Done;
    } else if (state_ != State::Done) {
        state_ = State::Error;
    }
}

// ---------------- WebSocketFramer ----------------

static std::string base64Encode(const uint8_t *data, size_t len) {
    static const char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTbl[(v >> 18) & 0x3F]);
        out.push_back(kTbl[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kTbl[(v >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kTbl[v & 0x3F] : '=');
    }
    return out;
}

void WebSocketFramer::reset() {
    state_ = State::Handshake;
    rxBuf_.clear();
    fragBuf_.clear();
    fragOpcode_ = 0;
}

std::string WebSocketFramer::buildUpgradeRequest(const ParsedUrl &url, const uint8_t random16[16]) {
    const std::string wsKey = base64Encode(random16, 16);
    std::string req = "GET " + url.path + " HTTP/1.1\r\n";
    req += "Host: " + url.host;
    if ((url.isTls && url.port != 443) || (!url.isTls && url.port != 80)) {
        req += ":" + std::to_string(url.port);
    }
    req += "\r\nUpgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + wsKey + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n\r\n";
    return req;
}

std::string WebSocketFramer::encodeFrame(uint8_t opcode, const std::string &payload, const uint8_t maskKey[4]) const {
    std::string frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<char>(0x80u | (opcode & 0x0Fu)));

    const uint64_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(0x80u | static_cast<uint8_t>(len)));
    } else if (len <= 0xFFFFu) {
        frame.push_back(static_cast<char>(0x80u | 126u));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80u | 127u));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((len >> shift) & 0xFF));
        }
    }
    for (int i = 0; i < 4; ++i) {
        frame.push_back(static_cast<char>(maskKey[i]));
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ maskKey[i & 3u]));
    }
    return frame;
}

std::string WebSocketFramer::encodeTextFrame(const std::string &payload, const uint8_t maskKey[4]) const {
    return encodeFrame(0x1, payload, maskKey);
}

std::string WebSocketFramer::encodePongFrame(const std::string &payload, const uint8_t maskKey[4]) const {
    return encodeFrame(0xA, payload, maskKey);
}

std::string WebSocketFramer::encodePingFrame(const uint8_t maskKey[4]) const {
    return encodeFrame(0x9, "", maskKey);
}

void WebSocketFramer::feed(const char *data,
                           size_t len,
                           const std::function<void(const std::string &)> &onText,
                           std::string &outTx) {
    if (state_ == State::Closed || state_ == State::Error) return;
    if (data && len > 0) rxBuf_.append(data, len);

    if (state_ == State::Handshake) {
        const size_t hdrEnd = rxBuf_.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) return;
        const std::string hdrs = lowerAscii(rxBuf_.substr(0, hdrEnd));
        rxBuf_.erase(0, hdrEnd + 4);
        if (hdrs.find(" 101 ") == std::string::npos) {
            state_ = State::Error;
            return;
        }
        state_ = State::Open;
    }

    while (state_ == State::Open && rxBuf_.size() >= 2) {
        const uint8_t b0 = static_cast<uint8_t>(rxBuf_[0]);
        const uint8_t b1 = static_cast<uint8_t>(rxBuf_[1]);
        const bool fin = (b0 & 0x80u) != 0;
        const uint8_t opcode = b0 & 0x0Fu;
        const bool masked = (b1 & 0x80u) != 0;
        uint64_t payloadLen = b1 & 0x7Fu;
        size_t hdrSize = 2;

        if (payloadLen == 126) {
            if (rxBuf_.size() < 4) break;
            payloadLen = (static_cast<uint64_t>(static_cast<uint8_t>(rxBuf_[2])) << 8) |
                         static_cast<uint64_t>(static_cast<uint8_t>(rxBuf_[3]));
            hdrSize = 4;
        } else if (payloadLen == 127) {
            if (rxBuf_.size() < 10) break;
            payloadLen = 0;
            for (int i = 0; i < 8; ++i) {
                payloadLen = (payloadLen << 8) | static_cast<uint8_t>(rxBuf_[2 + i]);
            }
            hdrSize = 10;
        }

        uint8_t maskKey[4] = {0, 0, 0, 0};
        if (masked) {
            if (rxBuf_.size() < hdrSize + 4) break;
            for (int i = 0; i < 4; ++i) maskKey[i] = static_cast<uint8_t>(rxBuf_[hdrSize + i]);
            hdrSize += 4;
        }

        if (rxBuf_.size() < hdrSize + payloadLen) break;

        std::string payload = rxBuf_.substr(hdrSize, static_cast<size_t>(payloadLen));
        rxBuf_.erase(0, hdrSize + static_cast<size_t>(payloadLen));

        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ maskKey[i & 3u]);
            }
        }

        if (opcode == 0x8) {
            state_ = State::Closed;
            break;
        } else if (opcode == 0x9) {
            const uint8_t kMask[4] = {0x12, 0x34, 0x56, 0x78};
            outTx += encodePongFrame(payload, kMask);
        } else if (opcode == 0xA) {
            // Pong ignored
        } else if (opcode == 0x1 || opcode == 0x2) {
            if (fin) {
                if (opcode == 0x1 && onText) onText(payload);
            } else {
                fragOpcode_ = opcode;
                fragBuf_ = std::move(payload);
            }
        } else if (opcode == 0x0) {
            fragBuf_ += payload;
            if (fin) {
                if (fragOpcode_ == 0x1 && onText) onText(fragBuf_);
                fragBuf_.clear();
                fragOpcode_ = 0;
            }
        }
    }
}

}  // namespace yad
