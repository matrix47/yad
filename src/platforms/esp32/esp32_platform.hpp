#pragma once

#include "yad/platform.hpp"
#include <unordered_map>

namespace yad {

class Esp32Platform : public IPlatform {
public:
    Esp32Platform() = default;
    ~Esp32Platform() override = default;

    void setToken(const std::string &token) { haToken_ = token; }
    void setFileOverride(const std::string &path, const std::string &content) { files_[path] = content; }

    std::unique_ptr<ITcpTlsStream> createStream() override;
    std::unique_ptr<ILocalSysProvider> createLocalSysProvider() override { return nullptr; }

    uint64_t nowMs() const override;
    std::time_t nowWallSec() const override;
    void fillRandom(uint8_t *buf, size_t len) override;
    std::string resolveEnv(const std::string &name) const override;
    std::string readFileText(const std::string &path) const override;

private:
    std::string haToken_;
    std::unordered_map<std::string, std::string> files_;
};

}  // namespace yad
