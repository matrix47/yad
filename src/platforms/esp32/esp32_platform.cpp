#include "esp32_platform.hpp"

#ifdef ESP_PLATFORM
#include <cerrno>
#include <cstring>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_tls.h>
#include <fcntl.h>
#include <fstream>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <sstream>

namespace yad {

namespace {

class EspTcpTlsStream : public ITcpTlsStream {
public:
    ~EspTcpTlsStream() override { close(); }

    bool beginConnect(const std::string &host, uint16_t port, bool useTls, bool tlsVerify) override {
        close();
        host_ = host;
        port_ = port;
        useTls_ = useTls;
        tlsVerify_ = tlsVerify;

        if (useTls_) {
            esp_tls_cfg_t cfg{};
            cfg.non_block = true;
            cfg.timeout_ms = 5000;
            cfg.skip_common_name = !tlsVerify_;
            tls_ = esp_tls_init();
            if (!tls_) {
                err_ = "esp_tls_init failed";
                state_ = State::Error;
                return false;
            }
            const int rc = esp_tls_conn_new_nonblock(host_.c_str(), static_cast<int>(host_.size()),
                                                     port_, &cfg, tls_);
            if (rc == 1) state_ = State::Open;
            else if (rc == 0) state_ = State::Handshaking;
            else {
                err_ = "esp_tls connect failed";
                close();
                state_ = State::Error;
                return false;
            }
            return true;
        }

        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char pstr[16];
        std::snprintf(pstr, sizeof(pstr), "%u", static_cast<unsigned>(port_));
        struct addrinfo *res = nullptr;
        if (lwip_getaddrinfo(host_.c_str(), pstr, &hints, &res) != 0 || !res) {
            err_ = "DNS failed";
            state_ = State::Error;
            return false;
        }
        sock_ = lwip_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock_ >= 0) {
            int flags = lwip_fcntl(sock_, F_GETFL, 0);
            lwip_fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
            const int rc = lwip_connect(sock_, res->ai_addr, res->ai_addrlen);
            if (rc == 0) state_ = State::Open;
            else if (errno == EINPROGRESS) state_ = State::Connecting;
            else {
                lwip_close(sock_);
                sock_ = -1;
            }
        }
        lwip_freeaddrinfo(res);
        if (sock_ < 0) {
            err_ = "socket connect failed";
            state_ = State::Error;
            return false;
        }
        return true;
    }

    State pollConnect() override {
        if (state_ == State::Open || state_ == State::Error || state_ == State::Idle) return state_;
        if (useTls_) {
            esp_tls_cfg_t cfg{};
            cfg.non_block = true;
            cfg.skip_common_name = !tlsVerify_;
            const int rc = esp_tls_conn_new_nonblock(host_.c_str(), static_cast<int>(host_.size()),
                                                     port_, &cfg, tls_);
            if (rc == 1) state_ = State::Open;
            else if (rc < 0) {
                err_ = "esp_tls handshake failed";
                close();
                state_ = State::Error;
            }
            return state_;
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock_, &wfds);
        struct timeval tv{0, 0};
        if (lwip_select(sock_ + 1, nullptr, &wfds, nullptr, &tv) > 0) {
            int soErr = 0;
            socklen_t len = sizeof(soErr);
            if (lwip_getsockopt(sock_, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0) {
                state_ = State::Open;
            } else {
                err_ = "connect error";
                close();
                state_ = State::Error;
            }
        }
        return state_;
    }

    int read(uint8_t *buf, size_t maxLen) override {
        if (state_ != State::Open) return -1;
        if (useTls_) {
            const ssize_t n = esp_tls_conn_read(tls_, buf, maxLen);
            if (n > 0) return static_cast<int>(n);
            if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) return 0;
            return -1;
        }
        const int n = lwip_recv(sock_, buf, maxLen, MSG_DONTWAIT);
        if (n > 0) return n;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }

    int write(const uint8_t *buf, size_t len) override {
        if (state_ != State::Open) return -1;
        if (len == 0) return 0;
        if (useTls_) {
            const ssize_t n = esp_tls_conn_write(tls_, buf, len);
            if (n >= 0) return static_cast<int>(n);
            if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) return 0;
            return -1;
        }
        const int n = lwip_send(sock_, buf, len, 0);
        if (n >= 0) return n;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }

    void close() override {
        if (tls_) {
            esp_tls_conn_destroy(tls_);
            tls_ = nullptr;
        }
        if (sock_ >= 0) {
            lwip_close(sock_);
            sock_ = -1;
        }
        if (state_ != State::Error) state_ = State::Idle;
    }

    State state() const override { return state_; }
    std::string lastError() const override { return err_; }

private:
    esp_tls_t *tls_ = nullptr;
    int sock_ = -1;
    std::string host_;
    uint16_t port_ = 80;
    bool useTls_ = false;
    bool tlsVerify_ = true;
    State state_ = State::Idle;
    std::string err_;
};

}  // namespace

std::unique_ptr<ITcpTlsStream> Esp32Platform::createStream() {
    return std::make_unique<EspTcpTlsStream>();
}

uint64_t Esp32Platform::nowMs() const {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

std::time_t Esp32Platform::nowWallSec() const {
    return std::time(nullptr);
}

void Esp32Platform::fillRandom(uint8_t *buf, size_t len) {
    esp_fill_random(buf, len);
}

std::string Esp32Platform::resolveEnv(const std::string &name) const {
    if (name == "HA_TOKEN" && !haToken_.empty()) return haToken_;
    return {};
}

std::string Esp32Platform::readFileText(const std::string &path) const {
    auto it = files_.find(path);
    if (it != files_.end()) return it->second;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

}  // namespace yad
#endif
