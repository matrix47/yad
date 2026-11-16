#pragma once

#include "yad/types.hpp"
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yad {

class NodeExporterParser {
public:
    // Optional prefix, e.g. "nas." so metrics become "nas.cpu.total", "nas.net.eth0.rx_bps", etc.
    void setPrefix(const std::string &prefix) { prefix_ = prefix; }
    const std::string &prefix() const { return prefix_; }

    void beginPoll();
    void feed(const char *data, size_t len);
    void endPoll(double nowSec, Snapshot &out, bool alsoWriteUnprefixed = false);

private:
    void processLine(const std::string &line);
    void writeMetrics(const std::string &pfx, double cpuPct, double dtSec, Snapshot &out) const;

    std::string prefix_;
    std::string lineBuf_;

    double cpuTotalSum_ = 0.0;
    double cpuIdleSum_ = 0.0;
    bool sawCpu_ = false;

    double memTotalBytes_ = 0.0;
    double memAvailBytes_ = 0.0;
    double swapTotalBytes_ = 0.0;
    double swapFreeBytes_ = 0.0;
    bool sawMem_ = false;

    double cpuTempC_ = -1.0;
    int cpuTempPriority_ = 99;
    std::unordered_map<std::string, std::string> hwmonChipNames_;
    struct HwmonTempSample {
        std::string chip;
        std::string sensor;
        double val = 0.0;
    };
    std::vector<HwmonTempSample> hwmonTemps_;

    struct NetSample {
        double rxBytes = 0.0;
        double txBytes = 0.0;
        bool hasRx = false;
        bool hasTx = false;
        bool up = false;
        bool hasUp = false;
    };
    std::unordered_map<std::string, NetSample> curNet_;

    double gpuUtilPct_ = -1.0;
    double gpuTempC_ = -1.0;
    double gpuMemUsedBytes_ = -1.0;
    double gpuMemTotalBytes_ = -1.0;
    double gpuMemFreeBytes_ = -1.0;

    struct NodeGpuSample {
        std::string model;
        double utilPct = -1.0;
        double tempC = -1.0;
        double memUsedBytes = -1.0;
        double memTotalBytes = -1.0;
        double powerW = -1.0;
        double powerLimitW = -1.0;
        double memUtilPct = -1.0;
        double encPct = -1.0;
        double decPct = -1.0;
        double fanPct = -1.0;
        int suspended = 0;
    };
    std::unordered_map<std::string, NodeGpuSample> curGpus_;

    double prevTimeSec_ = -1.0;
    double prevCpuTotal_ = 0.0;
    double prevCpuIdle_ = 0.0;
    bool hasPrevCpu_ = false;

    struct PrevNet {
        double rxBytes = 0.0;
        double txBytes = 0.0;
        bool valid = false;
    };
    std::unordered_map<std::string, PrevNet> prevNet_;
};

class HaProtocol {
public:
    void configure(const std::string &token, const std::vector<std::string> &watchedEntities);
    void onConnected();
    void onDisconnected();

    bool isConnected() const { return connected_; }
    bool authFailed() const { return authFailed_; }
    const std::string &lastError() const { return lastError_; }

    bool onTextMessage(const std::string &msg, std::vector<std::string> &outMessages);
    std::string buildCallService(const std::string &domain,
                                 const std::string &service,
                                 const std::string &entityId,
                                 const std::string &serviceDataJson = "{}");

    void mergeIntoSnapshot(Snapshot &out) const;

private:
    std::string token_;
    std::unordered_set<std::string> watched_;
    bool connected_ = false;
    bool authenticated_ = false;
    bool authFailed_ = false;
    int nextId_ = 1;
    int getStatesId_ = -1;
    std::string lastError_;
    Snapshot states_;
};

}  // namespace yad
