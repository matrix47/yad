#include "yad/posix_platform.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

namespace yad {

namespace {

class PosixTcpTlsStream : public ITcpTlsStream {
public:
    PosixTcpTlsStream() = default;
    ~PosixTcpTlsStream() override { close(); }

    bool beginConnect(const std::string &host, uint16_t port, bool useTls, bool tlsVerify) override {
        close();
        host_ = host;
        useTls_ = useTls;
        tlsVerify_ = tlsVerify;

        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        char portStr[16];
        std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

        struct addrinfo *res = nullptr;
        const int gai = ::getaddrinfo(host.c_str(), portStr, &hints, &res);
        if (gai != 0 || !res) {
            err_ = gai ? ::gai_strerror(gai) : "DNS lookup failed";
            state_ = State::Error;
            return false;
        }

        int sock = -1;
        for (struct addrinfo *rp = res; rp != nullptr; rp = rp->ai_next) {
            sock = ::socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, rp->ai_protocol);
            if (sock < 0) continue;
            const int rc = ::connect(sock, rp->ai_addr, rp->ai_addrlen);
            if (rc == 0 || errno == EINPROGRESS) {
                fd_ = sock;
                state_ = (rc == 0) ? (useTls_ ? State::Handshaking : State::Open) : State::Connecting;
                break;
            }
            ::close(sock);
            sock = -1;
        }
        ::freeaddrinfo(res);

        if (fd_ < 0) {
            err_ = std::strerror(errno);
            state_ = State::Error;
            return false;
        }

        if (state_ == State::Handshaking && !initTls()) {
            close();
            state_ = State::Error;
            return false;
        }
        return true;
    }

    State pollConnect() override {
        if (state_ == State::Open || state_ == State::Error || state_ == State::Idle) {
            return state_;
        }

        if (state_ == State::Connecting) {
            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            const int pr = ::poll(&pfd, 1, 0);
            if (pr < 0) {
                err_ = std::strerror(errno);
                close();
                state_ = State::Error;
                return state_;
            }
            if (pr == 0) return state_;

            int soErr = 0;
            socklen_t len = sizeof(soErr);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soErr, &len) < 0 || soErr != 0) {
                err_ = std::strerror(soErr ? soErr : errno);
                close();
                state_ = State::Error;
                return state_;
            }

            if (!useTls_) {
                state_ = State::Open;
                return state_;
            }
            if (!initTls()) {
                close();
                state_ = State::Error;
                return state_;
            }
            state_ = State::Handshaking;
        }

        if (state_ == State::Handshaking) {
            const int rc = ::mbedtls_ssl_handshake(&ssl_);
            if (rc == 0) {
                state_ = State::Open;
            } else if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char buf[128];
                ::mbedtls_strerror(rc, buf, sizeof(buf));
                err_ = buf;
                close();
                state_ = State::Error;
            }
        }
        return state_;
    }

    int read(uint8_t *buf, size_t maxLen) override {
        if (state_ != State::Open || fd_ < 0) return -1;
        if (!useTls_) {
            const ssize_t n = ::recv(fd_, buf, maxLen, MSG_DONTWAIT);
            if (n > 0) return static_cast<int>(n);
            if (n == 0) {
                err_ = "EOF";
                return -1;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            err_ = std::strerror(errno);
            return -1;
        }

        const int rc = ::mbedtls_ssl_read(&ssl_, buf, maxLen);
        if (rc > 0) return rc;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0) {
            err_ = "EOF";
            return -1;
        }
        char ebuf[128];
        ::mbedtls_strerror(rc, ebuf, sizeof(ebuf));
        err_ = ebuf;
        return -1;
    }

    int write(const uint8_t *buf, size_t len) override {
        if (state_ != State::Open || fd_ < 0) return -1;
        if (len == 0) return 0;
        if (!useTls_) {
            const ssize_t n = ::send(fd_, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n >= 0) return static_cast<int>(n);
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            err_ = std::strerror(errno);
            return -1;
        }

        const int rc = ::mbedtls_ssl_write(&ssl_, buf, len);
        if (rc >= 0) return rc;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        char ebuf[128];
        ::mbedtls_strerror(rc, ebuf, sizeof(ebuf));
        err_ = ebuf;
        return -1;
    }

    void close() override {
        if (tlsInit_) {
            ::mbedtls_ssl_free(&ssl_);
            ::mbedtls_ssl_config_free(&conf_);
            ::mbedtls_ctr_drbg_free(&ctrDrbg_);
            ::mbedtls_entropy_free(&entropy_);
            ::mbedtls_x509_crt_free(&caCert_);
            tlsInit_ = false;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (state_ != State::Error) {
            state_ = State::Idle;
        }
    }

    State state() const override { return state_; }
    std::string lastError() const override { return err_; }

private:
    static int bioSend(void *ctx, const unsigned char *buf, size_t len) {
        auto *self = static_cast<PosixTcpTlsStream *>(ctx);
        const ssize_t n = ::send(self->fd_, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n >= 0) return static_cast<int>(n);
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return -1;
    }

    static int bioRecv(void *ctx, unsigned char *buf, size_t len) {
        auto *self = static_cast<PosixTcpTlsStream *>(ctx);
        const ssize_t n = ::recv(self->fd_, buf, len, MSG_DONTWAIT);
        if (n > 0) return static_cast<int>(n);
        if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        return -1;
    }

    bool initTls() {
        ::mbedtls_ssl_init(&ssl_);
        ::mbedtls_ssl_config_init(&conf_);
        ::mbedtls_ctr_drbg_init(&ctrDrbg_);
        ::mbedtls_entropy_init(&entropy_);
        ::mbedtls_x509_crt_init(&caCert_);
        tlsInit_ = true;

        const char *pers = "yad_tls";
        if (::mbedtls_ctr_drbg_seed(&ctrDrbg_, ::mbedtls_entropy_func, &entropy_,
                                    reinterpret_cast<const unsigned char *>(pers), std::strlen(pers)) != 0) {
            err_ = "ctr_drbg_seed failed";
            return false;
        }

        if (::mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            err_ = "ssl_config_defaults failed";
            return false;
        }

        ::mbedtls_ssl_conf_rng(&conf_, ::mbedtls_ctr_drbg_random, &ctrDrbg_);
        if (tlsVerify_) {
            if (::mbedtls_x509_crt_parse_file(&caCert_, "/etc/ssl/certs/ca-certificates.crt") == 0) {
                ::mbedtls_ssl_conf_ca_chain(&conf_, &caCert_, nullptr);
            }
            ::mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
        } else {
            ::mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_NONE);
        }

        if (::mbedtls_ssl_setup(&ssl_, &conf_) != 0) {
            err_ = "ssl_setup failed";
            return false;
        }
        ::mbedtls_ssl_set_hostname(&ssl_, host_.c_str());
        ::mbedtls_ssl_set_bio(&ssl_, this, bioSend, bioRecv, nullptr);
        return true;
    }

    int fd_ = -1;
    bool useTls_ = false;
    bool tlsVerify_ = true;
    bool tlsInit_ = false;
    std::string host_;
    std::string err_;
    State state_ = State::Idle;

    mbedtls_ssl_context ssl_{};
    mbedtls_ssl_config conf_{};
    mbedtls_ctr_drbg_context ctrDrbg_{};
    mbedtls_entropy_context entropy_{};
    mbedtls_x509_crt caCert_{};
};

}  // namespace

std::unique_ptr<ITcpTlsStream> PosixPlatform::createStream() {
    return std::make_unique<PosixTcpTlsStream>();
}

}  // namespace yad
