#pragma once

#include "yad/platform.hpp"

namespace yad {

class PosixPlatform : public IPlatform {
public:
    explicit PosixPlatform(bool enableLocalSys = true);
    ~PosixPlatform() override;

    std::unique_ptr<ITcpTlsStream> createStream() override;
    std::unique_ptr<ILocalSysProvider> createLocalSysProvider() override;

    uint64_t nowMs() const override;
    std::time_t nowWallSec() const override;
    uint64_t nowWallMs() const override;
    void fillRandom(uint8_t *buf, size_t len) override;
    std::string resolveEnv(const std::string &name) const override;
    std::string readFileText(const std::string &path) const override;

private:
    bool enableLocalSys_ = true;
};

}  // namespace yad
