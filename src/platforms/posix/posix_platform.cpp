#include "yad/posix_platform.hpp"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace yad {

PosixPlatform::PosixPlatform(bool enableLocalSys) : enableLocalSys_(enableLocalSys) {}
PosixPlatform::~PosixPlatform() = default;

uint64_t PosixPlatform::nowMs() const {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

std::time_t PosixPlatform::nowWallSec() const {
    return std::time(nullptr);
}

uint64_t PosixPlatform::nowWallMs() const {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    return (ms > 0) ? static_cast<uint64_t>(ms) : 0ULL;
}

void PosixPlatform::fillRandom(uint8_t *buf, size_t len) {
    if (!buf || len == 0) return;
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            const ssize_t n = ::read(fd, buf + off, len - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
        ::close(fd);
        if (off == len) return;
    }
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(std::rand() & 0xFF);
    }
}

std::string PosixPlatform::resolveEnv(const std::string &name) const {
    const char *v = std::getenv(name.c_str());
    return v ? std::string(v) : std::string();
}

std::string PosixPlatform::readFileText(const std::string &path) const {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

}  // namespace yad
