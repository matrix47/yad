#pragma once

#include "yad/types.hpp"
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>

namespace yad {

class ITcpTlsStream {
public:
    enum class State { Idle, Connecting, Handshaking, Open, Error };

    virtual ~ITcpTlsStream() = default;

    virtual bool beginConnect(const std::string &host, uint16_t port, bool useTls, bool tlsVerify) = 0;
    // Advances non-blocking DNS/connect/TLS handshake.
    virtual State pollConnect() = 0;
    // Returns >0 bytes read, 0 if would-block, -1 on EOF/error.
    virtual int read(uint8_t *buf, size_t maxLen) = 0;
    // Returns >=0 bytes written (0 if would-block), -1 on error.
    virtual int write(const uint8_t *buf, size_t len) = 0;
    virtual void close() = 0;
    virtual State state() const = 0;
    virtual std::string lastError() const = 0;
};

class ILocalSysProvider {
public:
    virtual ~ILocalSysProvider() = default;
    virtual void sample(Snapshot &outSnapshot) = 0;
};

class IPlatform {
public:
    virtual ~IPlatform() = default;

    virtual std::unique_ptr<ITcpTlsStream> createStream() = 0;
    virtual std::unique_ptr<ILocalSysProvider> createLocalSysProvider() { return nullptr; }

    virtual uint64_t nowMs() const = 0;
    virtual std::time_t nowWallSec() const = 0;
    virtual uint64_t nowWallMs() const {
        return static_cast<uint64_t>(nowWallSec()) * 1000ULL;
    }
    virtual void fillRandom(uint8_t *buf, size_t len) = 0;
    virtual std::string resolveEnv(const std::string &name) const = 0;
    virtual std::string readFileText(const std::string &path) const = 0;
};

}  // namespace yad
