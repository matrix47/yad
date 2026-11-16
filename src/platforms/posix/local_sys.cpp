#include "yad/posix_platform.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace yad {

namespace {

static std::string readFirstLine(const std::string &path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return {};
    std::string line;
    std::getline(ifs, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

static std::string findCpuTempPath() {
    static const char *kPreferred[] = {"coretemp", "k10temp", "zenpower", "acpitz"};
    DIR *dir = ::opendir("/sys/class/hwmon");
    if (!dir) return {};
    std::vector<std::string> entries;
    while (struct dirent *ent = ::readdir(dir)) {
        if (std::strncmp(ent->d_name, "hwmon", 5) == 0) entries.emplace_back(ent->d_name);
    }
    ::closedir(dir);

    for (const char *pref : kPreferred) {
        for (const auto &entry : entries) {
            const std::string base = "/sys/class/hwmon/" + entry;
            if (readFirstLine(base + "/name") == pref) {
                const std::string temp1 = base + "/temp1_input";
                std::ifstream test(temp1);
                if (test.good()) return temp1;
            }
        }
    }
    return {};
}

struct NvmlUtilization {
    unsigned int gpu;
    unsigned int memory;
};

struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

class LinuxLocalSysProvider : public ILocalSysProvider {
public:
    LinuxLocalSysProvider() {
        cpuTempPath_ = findCpuTempPath();
        lastNetTime_ = std::chrono::steady_clock::now();
        Snapshot warmup;
        sample(warmup);
    }

    ~LinuxLocalSysProvider() override {
        if (nvmlReady_ && fnShutdown_) fnShutdown_();
        if (nvmlHandle_) ::dlclose(nvmlHandle_);
    }

    void sample(Snapshot &out) override {
        const auto now = std::chrono::steady_clock::now();
        const double dtSec = std::max(0.05, std::chrono::duration<double>(now - lastNetTime_).count());
        lastNetTime_ = now;

        sampleCpu(out);
        sampleMemory(out);
        sampleGpu(out);
        sampleNetwork(out, dtSec);
    }

private:
    void initNvml() {
        nvmlAttempted_ = true;
        nvmlHandle_ = ::dlopen("libnvidia-ml.so.1", RTLD_LAZY);
        if (!nvmlHandle_) nvmlHandle_ = ::dlopen("libnvidia-ml.so", RTLD_LAZY);
        if (!nvmlHandle_) return;

        fnInit_ = reinterpret_cast<int (*)()>(::dlsym(nvmlHandle_, "nvmlInit_v2"));
        fnShutdown_ = reinterpret_cast<int (*)()>(::dlsym(nvmlHandle_, "nvmlShutdown"));
        fnGetHandle_ = reinterpret_cast<int (*)(unsigned int, void **)>(::dlsym(nvmlHandle_, "nvmlDeviceGetHandleByIndex_v2"));
        fnGetUtil_ = reinterpret_cast<int (*)(void *, void *)>(::dlsym(nvmlHandle_, "nvmlDeviceGetUtilizationRates"));
        fnGetMem_ = reinterpret_cast<int (*)(void *, void *)>(::dlsym(nvmlHandle_, "nvmlDeviceGetMemoryInfo"));
        fnGetTemp_ = reinterpret_cast<int (*)(void *, int, unsigned int *)>(::dlsym(nvmlHandle_, "nvmlDeviceGetTemperature"));

        if (fnInit_ && fnGetHandle_ && fnGetUtil_ && fnInit_() == 0) {
            if (fnGetHandle_(0, &nvmlDev_) == 0 && nvmlDev_ != nullptr) nvmlReady_ = true;
        }
    }

    void sampleCpu(Snapshot &out) {
        const std::string line = readFirstLine("/proc/stat");
        std::istringstream iss(line);
        std::string tag;
        iss >> tag;
        double cpuPct = 0.0;
        if (tag == "cpu") {
            std::vector<uint64_t> cols;
            uint64_t v = 0;
            while (iss >> v) cols.push_back(v);
            if (cols.size() >= 4) {
                uint64_t total = 0;
                for (uint64_t c : cols) total += c;
                const uint64_t idle = cols[3] + (cols.size() > 4 ? cols[4] : 0);
                const uint64_t active = total - idle;
                if (hasPrevCpu_ && total > prevCpuTotal_) {
                    cpuPct = std::clamp((100.0 * (active - prevCpuActive_)) / double(total - prevCpuTotal_), 0.0, 100.0);
                }
                prevCpuActive_ = active;
                prevCpuTotal_ = total;
                hasPrevCpu_ = true;
            }
        }

        out.set("cpu.total", cpuPct);
        char buf[48];
        if (!cpuTempPath_.empty()) {
            const std::string ts = readFirstLine(cpuTempPath_);
            char *end = nullptr;
            const double milliC = std::strtod(ts.c_str(), &end);
            if (end && end != ts.c_str() && milliC > 0) {
                const double tempC = milliC / 1000.0;
                out.set("cpu.temp", tempC);
                std::snprintf(buf, sizeof(buf), "%d%% %d°C",
                              static_cast<int>(std::llround(cpuPct)),
                              static_cast<int>(std::llround(tempC)));
                out.set("cpu.detail", buf);
                return;
            }
        }
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::llround(cpuPct)));
        out.set("cpu.detail", buf);
    }

    void sampleMemory(Snapshot &out) {
        std::ifstream ifs("/proc/meminfo");
        if (!ifs.is_open()) return;
        uint64_t memTotalKb = 0, memAvailKb = 0, swapTotalKb = 0, swapFreeKb = 0;
        std::string line;
        while (std::getline(ifs, line)) {
            std::istringstream iss(line);
            std::string key;
            uint64_t val = 0;
            if (!(iss >> key >> val)) continue;
            if (key == "MemTotal:") memTotalKb = val;
            else if (key == "MemAvailable:") memAvailKb = val;
            else if (key == "SwapTotal:") swapTotalKb = val;
            else if (key == "SwapFree:") swapFreeKb = val;
        }
        if (memTotalKb > 0) {
            const uint64_t usedKb = memTotalKb > memAvailKb ? (memTotalKb - memAvailKb) : 0;
            const double pct = (100.0 * usedKb) / double(memTotalKb);
            const double usedGb = usedKb / (1024.0 * 1024.0);
            const double totalGb = memTotalKb / (1024.0 * 1024.0);
            out.set("mem.used_pct", pct);
            out.set("mem.used_gb", usedGb);
            out.set("mem.total_gb", totalGb);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f/%.0fG", usedGb, totalGb);
            out.set("mem.detail", buf);
        }
        if (swapTotalKb > 0) {
            const uint64_t usedKb = swapTotalKb > swapFreeKb ? (swapTotalKb - swapFreeKb) : 0;
            const double pct = (100.0 * usedKb) / double(swapTotalKb);
            const double usedGb = usedKb / (1024.0 * 1024.0);
            const double totalGb = swapTotalKb / (1024.0 * 1024.0);
            out.set("swap.used_pct", pct);
            out.set("swap.used_gb", usedGb);
            out.set("swap.total_gb", totalGb);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f/%.0fG", usedGb, totalGb);
            out.set("swap.detail", buf);
        } else {
            out.set("swap.used_pct", 0.0);
            out.set("swap.detail", "0G");
        }
    }

    void sampleGpu(Snapshot &out) {
        if (!nvmlAttempted_) initNvml();
        if (nvmlReady_) {
            NvmlUtilization util{0, 0};
            if (fnGetUtil_(nvmlDev_, &util) == 0) out.set("gpu.util", double(util.gpu));
            double tempC = -1.0;
            if (fnGetTemp_) {
                unsigned int t = 0;
                if (fnGetTemp_(nvmlDev_, 0, &t) == 0) {
                    tempC = double(t);
                    out.set("gpu.temp", tempC);
                }
            }
            if (fnGetMem_) {
                NvmlMemory mem{0, 0, 0};
                if (fnGetMem_(nvmlDev_, &mem) == 0 && mem.total > 0) {
                    const double pct = (100.0 * mem.used) / double(mem.total);
                    const double usedGb = mem.used / (1024.0 * 1024.0 * 1024.0);
                    const double totalGb = mem.total / (1024.0 * 1024.0 * 1024.0);
                    out.set("gpu.mem_pct", pct);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.1f/%.0fG", usedGb, totalGb);
                    out.set("gpu.mem_detail", buf);
                }
            }
            char buf[48];
            if (tempC >= 0) std::snprintf(buf, sizeof(buf), "%u%% %d°C", util.gpu, static_cast<int>(std::llround(tempC)));
            else std::snprintf(buf, sizeof(buf), "%u%%", util.gpu);
            out.set("gpu.detail", buf);
            return;
        }

        DIR *dir = ::opendir("/sys/class/drm");
        if (dir) {
            while (struct dirent *ent = ::readdir(dir)) {
                if (std::strncmp(ent->d_name, "card", 4) != 0 || std::strchr(ent->d_name, '-')) continue;
                const std::string devBase = std::string("/sys/class/drm/") + ent->d_name + "/device";
                const std::string busyStr = readFirstLine(devBase + "/gpu_busy_percent");
                if (!busyStr.empty()) {
                    const double busy = std::strtod(busyStr.c_str(), nullptr);
                    out.set("gpu.util", busy);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::llround(busy)));
                    out.set("gpu.detail", buf);

                    const std::string uStr = readFirstLine(devBase + "/mem_info_vram_used");
                    const std::string tStr = readFirstLine(devBase + "/mem_info_vram_total");
                    if (!uStr.empty() && !tStr.empty()) {
                        const uint64_t vu = std::strtoull(uStr.c_str(), nullptr, 10);
                        const uint64_t vt = std::strtoull(tStr.c_str(), nullptr, 10);
                        if (vt > 0) {
                            out.set("gpu.mem_pct", (100.0 * vu) / double(vt));
                            std::snprintf(buf, sizeof(buf), "%.1f/%.0fG",
                                          vu / (1024.0 * 1024.0 * 1024.0),
                                          vt / (1024.0 * 1024.0 * 1024.0));
                            out.set("gpu.mem_detail", buf);
                        }
                    }
                    ::closedir(dir);
                    return;
                }
            }
            ::closedir(dir);
        }

        out.set("gpu.util", 0.0);
        out.set("gpu.detail", "N/A");
        out.set("gpu.mem_pct", 0.0);
        out.set("gpu.mem_detail", "N/A");
    }

    void sampleNetwork(Snapshot &out, double dtSec) {
        DIR *dir = ::opendir("/sys/class/net");
        if (!dir) return;
        while (struct dirent *ent = ::readdir(dir)) {
            if (ent->d_name[0] == '.' || std::strcmp(ent->d_name, "lo") == 0) continue;
            const std::string iface = ent->d_name;
            const std::string base = "/sys/class/net/" + iface;
            const std::string oper = readFirstLine(base + "/operstate");
            const std::string carrier = readFirstLine(base + "/carrier");
            const bool isUp = (oper == "up") || (carrier == "1");

            const uint64_t rx = std::strtoull(readFirstLine(base + "/statistics/rx_bytes").c_str(), nullptr, 10);
            const uint64_t tx = std::strtoull(readFirstLine(base + "/statistics/tx_bytes").c_str(), nullptr, 10);
            double rxBps = 0.0, txBps = 0.0;
            auto it = prevNet_.find(iface);
            if (it != prevNet_.end()) {
                if (rx >= it->second.rx) rxBps = (rx - it->second.rx) / dtSec;
                if (tx >= it->second.tx) txBps = (tx - it->second.tx) / dtSec;
            }
            prevNet_[iface] = {rx, tx};

            const std::string pfx = "net." + iface + ".";
            out.set(pfx + "up", isUp);
            out.set(pfx + "rx_bps", rxBps);
            out.set(pfx + "tx_bps", txBps);
        }
        ::closedir(dir);
    }

    std::string cpuTempPath_;
    uint64_t prevCpuActive_ = 0;
    uint64_t prevCpuTotal_ = 0;
    bool hasPrevCpu_ = false;
    struct NetPrev { uint64_t rx = 0; uint64_t tx = 0; };
    std::unordered_map<std::string, NetPrev> prevNet_;
    std::chrono::steady_clock::time_point lastNetTime_;

    void *nvmlHandle_ = nullptr;
    bool nvmlAttempted_ = false;
    bool nvmlReady_ = false;
    void *nvmlDev_ = nullptr;
    int (*fnInit_)() = nullptr;
    int (*fnShutdown_)() = nullptr;
    int (*fnGetHandle_)(unsigned int, void **) = nullptr;
    int (*fnGetUtil_)(void *, void *) = nullptr;
    int (*fnGetMem_)(void *, void *) = nullptr;
    int (*fnGetTemp_)(void *, int, unsigned int *) = nullptr;
};

}  // namespace

std::unique_ptr<ILocalSysProvider> PosixPlatform::createLocalSysProvider() {
    if (!enableLocalSys_) return nullptr;
    return std::make_unique<LinuxLocalSysProvider>();
}

}  // namespace yad
