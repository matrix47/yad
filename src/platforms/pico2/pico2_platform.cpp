#include "pico2_platform.hpp"

#ifdef PICO_RP2350
#include <pico/rand.h>
#include <pico/stdlib.h>

namespace yad {

// Note: Wire up lwIP altcp / altcp_tls (CYW43 Pico 2 W) here when integrating into a Pico SDK app.
// YadEngine's HTTP/1.1 & RFC 6455 WebSocket parsers run unmodified over ITcpTlsStream.

uint64_t Pico2Platform::nowMs() const {
    return to_ms_since_boot(get_absolute_time());
}

std::time_t Pico2Platform::nowWallSec() const {
    return std::time(nullptr);
}

void Pico2Platform::fillRandom(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i += 4) {
        const uint32_t r = get_rand_32();
        for (size_t b = 0; b < 4 && (i + b) < len; ++b) {
            buf[i + b] = static_cast<uint8_t>((r >> (b * 8)) & 0xFF);
        }
    }
}

std::string Pico2Platform::resolveEnv(const std::string &name) const {
    if (name == "HA_TOKEN" && !haToken_.empty()) return haToken_;
    return {};
}

std::string Pico2Platform::readFileText(const std::string &path) const {
    auto it = files_.find(path);
    return (it != files_.end()) ? it->second : std::string();
}

std::unique_ptr<ITcpTlsStream> Pico2Platform::createStream() {
    return nullptr;
}

}  // namespace yad
#endif
