#include "yad/providers.hpp"

#if __has_include(<cjson/cJSON.h>)
#include <cjson/cJSON.h>
#else
#include <cJSON.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace yad {

static bool extractLabel(const std::string &labels, const char *key, std::string &outVal) {
    const std::string pat = std::string(key) + "=\"";
    const size_t pos = labels.find(pat);
    if (pos == std::string::npos) return false;
    const size_t valStart = pos + pat.size();
    const size_t valEnd = labels.find('"', valStart);
    if (valEnd == std::string::npos) return false;
    outVal = labels.substr(valStart, valEnd - valStart);
    return true;
}

static bool parsePromLine(const std::string &line,
                          std::string &outName,
                          std::string &outLabels,
                          double &outVal) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i] == '#') return false;

    const size_t nameStart = i;
    while (i < line.size() && line[i] != '{' && line[i] != ' ' && line[i] != '\t') ++i;
    outName = line.substr(nameStart, i - nameStart);
    if (outName.empty()) return false;

    outLabels.clear();
    if (i < line.size() && line[i] == '{') {
        const size_t lStart = i + 1;
        size_t lEnd = lStart;
        bool inQuote = false;
        while (lEnd < line.size()) {
            if (line[lEnd] == '"' && (lEnd == lStart || line[lEnd - 1] != '\\')) {
                inQuote = !inQuote;
            } else if (line[lEnd] == '}' && !inQuote) {
                break;
            }
            ++lEnd;
        }
        if (lEnd >= line.size()) return false;
        outLabels = line.substr(lStart, lEnd - lStart);
        i = lEnd + 1;
    }

    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size()) return false;

    char *end = nullptr;
    outVal = std::strtod(line.c_str() + i, &end);
    return end != line.c_str() + i && std::isfinite(outVal);
}

void NodeExporterParser::beginPoll() {
    lineBuf_.clear();
    cpuTotalSum_ = 0.0;
    cpuIdleSum_ = 0.0;
    sawCpu_ = false;
    memTotalBytes_ = 0.0;
    memAvailBytes_ = 0.0;
    swapTotalBytes_ = 0.0;
    swapFreeBytes_ = 0.0;
    sawMem_ = false;
    cpuTempC_ = -1.0;
    cpuTempPriority_ = 99;
    hwmonChipNames_.clear();
    hwmonTemps_.clear();
    curNet_.clear();
    gpuUtilPct_ = -1.0;
    gpuTempC_ = -1.0;
    gpuMemUsedBytes_ = -1.0;
    gpuMemTotalBytes_ = -1.0;
    gpuMemFreeBytes_ = -1.0;
    curGpus_.clear();
}

void NodeExporterParser::feed(const char *data, size_t len) {
    if (!data || len == 0) return;
    lineBuf_.append(data, len);
    size_t start = 0;
    while (true) {
        const size_t nl = lineBuf_.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = lineBuf_.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        processLine(line);
        start = nl + 1;
    }
    if (start > 0) lineBuf_.erase(0, start);
}

void NodeExporterParser::processLine(const std::string &line) {
    std::string name;
    std::string labels;
    double val = 0.0;
    if (!parsePromLine(line, name, labels, val)) return;

    if (name == "node_cpu_seconds_total") {
        sawCpu_ = true;
        cpuTotalSum_ += val;
        std::string mode;
        if (extractLabel(labels, "mode", mode) && (mode == "idle" || mode == "iowait")) {
            cpuIdleSum_ += val;
        }
        return;
    }
    if (name == "node_memory_MemTotal_bytes") { sawMem_ = true; memTotalBytes_ = val; return; }
    if (name == "node_memory_MemAvailable_bytes") { sawMem_ = true; memAvailBytes_ = val; return; }
    if (name == "node_memory_SwapTotal_bytes") { sawMem_ = true; swapTotalBytes_ = val; return; }
    if (name == "node_memory_SwapFree_bytes") { sawMem_ = true; swapFreeBytes_ = val; return; }

    if (name == "node_hwmon_chip_names") {
        std::string chip, chipName;
        if (extractLabel(labels, "chip", chip) && extractLabel(labels, "chip_name", chipName)) {
            hwmonChipNames_[chip] = chipName;
        }
        return;
    }

    if (name == "node_hwmon_temp_celsius") {
        std::string chip, sensor;
        extractLabel(labels, "chip", chip);
        extractLabel(labels, "sensor", sensor);
        hwmonTemps_.push_back({chip, sensor, val});
        return;
    }

    if (name == "node_network_up" || name == "node_network_receive_bytes_total" ||
        name == "node_network_transmit_bytes_total") {
        std::string dev;
        if (!extractLabel(labels, "device", dev) || dev.empty() || dev == "lo") return;
        auto &ns = curNet_[dev];
        if (name == "node_network_up") { ns.up = (val > 0.5); ns.hasUp = true; }
        else if (name == "node_network_receive_bytes_total") { ns.rxBytes = val; ns.hasRx = true; }
        else { ns.txBytes = val; ns.hasTx = true; }
        return;
    }

    if (name.compare(0, 9, "node_gpu_") == 0) {
        std::string gpuIdx = "0";
        extractLabel(labels, "gpu", gpuIdx);
        auto &g = curGpus_[gpuIdx];
        if (name == "node_gpu_info") {
            extractLabel(labels, "model", g.model);
        } else if (name == "node_gpu_suspended") {
            g.suspended = (val > 0.5) ? 1 : 0;
        } else if (name == "node_gpu_utilization_ratio") {
            g.utilPct = val * 100.0;
        } else if (name == "node_gpu_memory_used_bytes") {
            g.memUsedBytes = val;
        } else if (name == "node_gpu_memory_total_bytes") {
            g.memTotalBytes = val;
        } else if (name == "node_gpu_temperature_celsius") {
            g.tempC = val;
        } else if (name == "node_gpu_power_watts") {
            g.powerW = val;
        } else if (name == "node_gpu_power_limit_watts") {
            g.powerLimitW = val;
        } else if (name == "node_gpu_memory_utilization_ratio") {
            g.memUtilPct = val * 100.0;
        } else if (name == "node_gpu_encoder_utilization_ratio") {
            g.encPct = val * 100.0;
        } else if (name == "node_gpu_decoder_utilization_ratio") {
            g.decPct = val * 100.0;
        } else if (name == "node_gpu_fan_speed_ratio") {
            g.fanPct = val * 100.0;
        }
        return;
    }

    if (name == "nvidia_smi_utilization_gpu_ratio") gpuUtilPct_ = val * 100.0;
    else if (name == "nvidia_gpu_duty_cycle" || name == "DCGM_FI_DEV_GPU_UTIL") gpuUtilPct_ = val;
    else if (name == "nvidia_smi_temperature_gpu" || name == "nvidia_gpu_temperature_celsius" ||
             name == "DCGM_FI_DEV_GPU_TEMP") gpuTempC_ = val;
    else if (name == "nvidia_smi_memory_used_bytes" || name == "nvidia_gpu_memory_used_bytes") gpuMemUsedBytes_ = val;
    else if (name == "nvidia_smi_memory_total_bytes" || name == "nvidia_gpu_memory_total_bytes") gpuMemTotalBytes_ = val;
    else if (name == "DCGM_FI_DEV_FB_USED") gpuMemUsedBytes_ = val * 1024.0 * 1024.0;
    else if (name == "DCGM_FI_DEV_FB_FREE") gpuMemFreeBytes_ = val * 1024.0 * 1024.0;
}

void NodeExporterParser::writeMetrics(const std::string &pfx, double cpuPct, double dtSec, Snapshot &out) const {
    if (sawCpu_) {
        out.set(pfx + "cpu.total", cpuPct);
        char buf[48];
        if (cpuTempC_ >= 0.0) {
            out.set(pfx + "cpu.temp", cpuTempC_);
            std::snprintf(buf, sizeof(buf), "%d%% %d°C",
                          static_cast<int>(std::llround(cpuPct)),
                          static_cast<int>(std::llround(cpuTempC_)));
        } else {
            std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::llround(cpuPct)));
        }
        out.set(pfx + "cpu.detail", buf);
    }

    if (sawMem_ && memTotalBytes_ > 0.0) {
        const double usedBytes = std::max(0.0, memTotalBytes_ - memAvailBytes_);
        const double pct = (100.0 * usedBytes) / memTotalBytes_;
        const double usedGb = usedBytes / (1024.0 * 1024.0 * 1024.0);
        const double totalGb = memTotalBytes_ / (1024.0 * 1024.0 * 1024.0);
        out.set(pfx + "mem.used_pct", pct);
        out.set(pfx + "mem.used_gb", usedGb);
        out.set(pfx + "mem.total_gb", totalGb);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f/%.0fG", usedGb, totalGb);
        out.set(pfx + "mem.detail", buf);

        if (swapTotalBytes_ > 0.0) {
            const double swapUsed = std::max(0.0, swapTotalBytes_ - swapFreeBytes_);
            const double swapPct = (100.0 * swapUsed) / swapTotalBytes_;
            const double sUsedGb = swapUsed / (1024.0 * 1024.0 * 1024.0);
            const double sTotalGb = swapTotalBytes_ / (1024.0 * 1024.0 * 1024.0);
            out.set(pfx + "swap.used_pct", swapPct);
            out.set(pfx + "swap.used_gb", sUsedGb);
            out.set(pfx + "swap.total_gb", sTotalGb);
            std::snprintf(buf, sizeof(buf), "%.1f/%.0fG", sUsedGb, sTotalGb);
            out.set(pfx + "swap.detail", buf);
        } else {
            out.set(pfx + "swap.used_pct", 0.0);
            out.set(pfx + "swap.detail", "0G");
        }
    }

    for (const auto &[dev, ns] : curNet_) {
        const std::string np = pfx + "net." + dev + ".";
        out.set(np + "up", ns.hasUp ? ns.up : true);
        double rxBps = 0.0, txBps = 0.0;
        auto it = prevNet_.find(dev);
        if (it != prevNet_.end() && it->second.valid) {
            if (ns.hasRx && ns.rxBytes >= it->second.rxBytes) rxBps = (ns.rxBytes - it->second.rxBytes) / dtSec;
            if (ns.hasTx && ns.txBytes >= it->second.txBytes) txBps = (ns.txBytes - it->second.txBytes) / dtSec;
        }
        out.set(np + "rx_bps", rxBps);
        out.set(np + "tx_bps", txBps);
    }

    if (!curGpus_.empty()) {
        for (const auto &[idx, g] : curGpus_) {
            const std::string gp = pfx + "gpu." + idx + ".";
            if (!g.model.empty()) out.set(gp + "model", g.model);
            out.set(gp + "suspended", g.suspended != 0);
            if (g.utilPct >= 0.0) {
                out.set(gp + "util", g.utilPct);
                char buf[48];
                if (g.tempC >= 0.0) {
                    out.set(gp + "temp", g.tempC);
                    std::snprintf(buf, sizeof(buf), "%d%% %d°C",
                                  static_cast<int>(std::llround(g.utilPct)),
                                  static_cast<int>(std::llround(g.tempC)));
                } else {
                    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::llround(g.utilPct)));
                }
                out.set(gp + "detail", buf);
            }
            if (g.memUsedBytes >= 0.0 && g.memTotalBytes > 0.0) {
                const double pct = (100.0 * g.memUsedBytes) / g.memTotalBytes;
                const double usedGb = g.memUsedBytes / (1024.0 * 1024.0 * 1024.0);
                const double totalGb = g.memTotalBytes / (1024.0 * 1024.0 * 1024.0);
                out.set(gp + "mem_pct", pct);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.1f/%.0fG", usedGb, totalGb);
                out.set(gp + "mem_detail", buf);
            }
            if (g.powerW >= 0.0) {
                out.set(gp + "power_w", g.powerW);
                char buf[32];
                if (g.powerLimitW > 0.0) {
                    out.set(gp + "power_limit_w", g.powerLimitW);
                    out.set(gp + "power_pct", (100.0 * g.powerW) / g.powerLimitW);
                    std::snprintf(buf, sizeof(buf), "%.0f/%.0fW", g.powerW, g.powerLimitW);
                } else {
                    std::snprintf(buf, sizeof(buf), "%.0fW", g.powerW);
                }
                out.set(gp + "power_detail", buf);
            }
            if (g.memUtilPct >= 0.0) out.set(gp + "mem_util", g.memUtilPct);
            if (g.encPct >= 0.0) out.set(gp + "enc_pct", g.encPct);
            if (g.decPct >= 0.0) out.set(gp + "dec_pct", g.decPct);
            if (g.fanPct >= 0.0) out.set(gp + "fan_pct", g.fanPct);

            if (idx == "0") {
                if (!g.model.empty()) out.set(pfx + "gpu.model", g.model);
                if (g.utilPct >= 0.0) {
                    out.set(pfx + "gpu.util", g.utilPct);
                    out.set(pfx + "gpu.detail", out.getString(gp + "detail"));
                }
                if (g.tempC >= 0.0) out.set(pfx + "gpu.temp", g.tempC);
                if (g.memUsedBytes >= 0.0 && g.memTotalBytes > 0.0) {
                    out.set(pfx + "gpu.mem_pct", out.getDouble(gp + "mem_pct"));
                    out.set(pfx + "gpu.mem_detail", out.getString(gp + "mem_detail"));
                }
                if (g.powerW >= 0.0) {
                    out.set(pfx + "gpu.power_w", g.powerW);
                    out.set(pfx + "gpu.power_detail", out.getString(gp + "power_detail"));
                }
            }
        }
    } else {
        if (gpuUtilPct_ >= 0.0) {
            out.set(pfx + "gpu.util", gpuUtilPct_);
            char buf[48];
            if (gpuTempC_ >= 0.0) {
                out.set(pfx + "gpu.temp", gpuTempC_);
                std::snprintf(buf, sizeof(buf), "%d%% %d°C",
                              static_cast<int>(std::llround(gpuUtilPct_)),
                              static_cast<int>(std::llround(gpuTempC_)));
            } else {
                std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::llround(gpuUtilPct_)));
            }
            out.set(pfx + "gpu.detail", buf);
        } else if (gpuTempC_ >= 0.0) {
            out.set(pfx + "gpu.temp", gpuTempC_);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d°C", static_cast<int>(std::llround(gpuTempC_)));
            out.set(pfx + "gpu.detail", buf);
        }

        if (gpuMemUsedBytes_ >= 0.0 && (gpuMemTotalBytes_ > 0.0 || gpuMemFreeBytes_ >= 0.0)) {
            const double total = (gpuMemTotalBytes_ > 0.0) ? gpuMemTotalBytes_ : (gpuMemUsedBytes_ + gpuMemFreeBytes_);
            if (total > 0.0) {
                const double pct = (100.0 * gpuMemUsedBytes_) / total;
                const double usedGb = gpuMemUsedBytes_ / (1024.0 * 1024.0 * 1024.0);
                const double totalGb = total / (1024.0 * 1024.0 * 1024.0);
                out.set(pfx + "gpu.mem_pct", pct);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.1f/%.0fG", usedGb, totalGb);
                out.set(pfx + "gpu.mem_detail", buf);
            }
        }
    }
}

void NodeExporterParser::endPoll(double nowSec, Snapshot &out, bool alsoWriteUnprefixed) {
    if (!lineBuf_.empty()) {
        if (lineBuf_.back() == '\r') lineBuf_.pop_back();
        processLine(lineBuf_);
        lineBuf_.clear();
    }

    const double dtSec = (prevTimeSec_ > 0.0 && nowSec > prevTimeSec_)
                             ? std::max(0.05, nowSec - prevTimeSec_)
                             : 1.0;
    prevTimeSec_ = nowSec;

    double cpuPct = 0.0;
    if (sawCpu_) {
        if (hasPrevCpu_ && cpuTotalSum_ > prevCpuTotal_) {
            const double dTotal = cpuTotalSum_ - prevCpuTotal_;
            const double dIdle = std::max(0.0, cpuIdleSum_ - prevCpuIdle_);
            const double dActive = std::max(0.0, dTotal - dIdle);
            cpuPct = std::clamp((100.0 * dActive) / dTotal, 0.0, 100.0);
        }
        prevCpuTotal_ = cpuTotalSum_;
        prevCpuIdle_ = cpuIdleSum_;
        hasPrevCpu_ = true;
    }

    for (const auto &ht : hwmonTemps_) {
        auto it = hwmonChipNames_.find(ht.chip);
        const std::string &resolved = (it != hwmonChipNames_.end()) ? it->second : ht.chip;
        int prio = 99;
        if (resolved.find("coretemp") != std::string::npos) prio = 0;
        else if (resolved.find("k10temp") != std::string::npos) prio = 1;
        else if (resolved.find("zenpower") != std::string::npos) prio = 2;
        else if (resolved.find("acpitz") != std::string::npos) prio = 3;
        if (prio < 99 && (prio < cpuTempPriority_ || (prio == cpuTempPriority_ && ht.sensor == "temp1"))) {
            cpuTempPriority_ = prio;
            cpuTempC_ = ht.val;
        }
        if (gpuTempC_ < 0.0 && resolved.find("amdgpu") != std::string::npos && ht.sensor == "temp1") {
            gpuTempC_ = ht.val;
        }
    }

    writeMetrics(prefix_, cpuPct, dtSec, out);
    if (alsoWriteUnprefixed && !prefix_.empty()) {
        writeMetrics("", cpuPct, dtSec, out);
    }

    for (const auto &[dev, ns] : curNet_) {
        prevNet_[dev] = {ns.rxBytes, ns.txBytes, true};
    }
}

// ---------------- HaProtocol ----------------

void HaProtocol::configure(const std::string &token, const std::vector<std::string> &watchedEntities) {
    token_ = token;
    watched_.clear();
    for (const auto &e : watchedEntities) {
        if (!e.empty()) watched_.insert(e);
    }
    authFailed_ = false;
    connected_ = false;
    authenticated_ = false;
    lastError_.clear();
}

void HaProtocol::onConnected() {
    authenticated_ = false;
    connected_ = false;
    lastError_.clear();
}

void HaProtocol::onDisconnected() {
    connected_ = false;
    authenticated_ = false;
}

static bool applyHaEntity(const cJSON *stateObj,
                          const std::unordered_set<std::string> &watched,
                          Snapshot &states) {
    if (!cJSON_IsObject(stateObj)) return false;
    const cJSON *entItem = cJSON_GetObjectItemCaseSensitive(stateObj, "entity_id");
    if (!cJSON_IsString(entItem) || !entItem->valuestring) return false;
    const std::string entityId = entItem->valuestring;
    if (!watched.empty() && watched.find(entityId) == watched.end()) return false;

    bool changed = false;
    const std::string baseKey = "ha." + entityId;
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(stateObj, "state");
    if (cJSON_IsString(st) && st->valuestring) {
        changed |= states.setIfChanged(baseKey, SnapshotValue::fromString(st->valuestring));
    } else if (cJSON_IsNumber(st)) {
        changed |= states.setIfChanged(baseKey, SnapshotValue::fromDouble(st->valuedouble));
    } else if (cJSON_IsBool(st)) {
        changed |= states.setIfChanged(baseKey, SnapshotValue::fromBool(cJSON_IsTrue(st)));
    }

    const cJSON *attrs = cJSON_GetObjectItemCaseSensitive(stateObj, "attributes");
    if (cJSON_IsObject(attrs)) {
        for (const cJSON *a = attrs->child; a != nullptr; a = a->next) {
            if (!a->string) continue;
            const std::string attrKey = baseKey + "." + a->string;
            if (cJSON_IsString(a) && a->valuestring) {
                changed |= states.setIfChanged(attrKey, SnapshotValue::fromString(a->valuestring));
            } else if (cJSON_IsNumber(a)) {
                changed |= states.setIfChanged(attrKey, SnapshotValue::fromDouble(a->valuedouble));
            } else if (cJSON_IsBool(a)) {
                changed |= states.setIfChanged(attrKey, SnapshotValue::fromBool(cJSON_IsTrue(a)));
            }
        }
    }
    return changed;
}

bool HaProtocol::onTextMessage(const std::string &msg, std::vector<std::string> &outMessages) {
    cJSON *obj = cJSON_Parse(msg.c_str());
    if (!obj || !cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        return false;
    }

    const cJSON *typeItem = cJSON_GetObjectItemCaseSensitive(obj, "type");
    const std::string type = (cJSON_IsString(typeItem) && typeItem->valuestring) ? typeItem->valuestring : "";

    auto emitJson = [&](cJSON *root) {
        if (char *printed = cJSON_PrintUnformatted(root)) {
            outMessages.emplace_back(printed);
            cJSON_free(printed);
        }
        cJSON_Delete(root);
    };

    if (type == "auth_required") {
        cJSON *auth = cJSON_CreateObject();
        cJSON_AddStringToObject(auth, "type", "auth");
        cJSON_AddStringToObject(auth, "access_token", token_.c_str());
        emitJson(auth);
        cJSON_Delete(obj);
        return false;
    }

    if (type == "auth_ok") {
        authenticated_ = true;
        connected_ = true;

        getStatesId_ = nextId_++;
        cJSON *gs = cJSON_CreateObject();
        cJSON_AddNumberToObject(gs, "id", getStatesId_);
        cJSON_AddStringToObject(gs, "type", "get_states");
        emitJson(gs);

        cJSON *sub = cJSON_CreateObject();
        cJSON_AddNumberToObject(sub, "id", nextId_++);
        cJSON_AddStringToObject(sub, "type", "subscribe_events");
        cJSON_AddStringToObject(sub, "event_type", "state_changed");
        emitJson(sub);

        cJSON_Delete(obj);
        return true;
    }

    if (type == "auth_invalid") {
        authFailed_ = true;
        connected_ = false;
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(obj, "message");
        lastError_ = (cJSON_IsString(m) && m->valuestring) ? m->valuestring : "Authentication invalid";
        cJSON_Delete(obj);
        return false;
    }

    if (type == "result") {
        const cJSON *idItem = cJSON_GetObjectItemCaseSensitive(obj, "id");
        const cJSON *okItem = cJSON_GetObjectItemCaseSensitive(obj, "success");
        if (cJSON_IsNumber(idItem) && static_cast<int>(idItem->valuedouble) == getStatesId_ && cJSON_IsTrue(okItem)) {
            bool changed = false;
            const cJSON *res = cJSON_GetObjectItemCaseSensitive(obj, "result");
            if (cJSON_IsArray(res)) {
                const int n = cJSON_GetArraySize(res);
                for (int i = 0; i < n; ++i) {
                    changed |= applyHaEntity(cJSON_GetArrayItem(res, i), watched_, states_);
                }
            }
            cJSON_Delete(obj);
            return changed;
        }
        cJSON_Delete(obj);
        return false;
    }

    if (type == "event") {
        const cJSON *ev = cJSON_GetObjectItemCaseSensitive(obj, "event");
        const cJSON *evType = ev ? cJSON_GetObjectItemCaseSensitive(ev, "event_type") : nullptr;
        if (cJSON_IsString(evType) && evType->valuestring &&
            std::string(evType->valuestring) == "state_changed") {
            const cJSON *data = cJSON_GetObjectItemCaseSensitive(ev, "data");
            const cJSON *newState = data ? cJSON_GetObjectItemCaseSensitive(data, "new_state") : nullptr;
            if (cJSON_IsObject(newState)) {
                const bool changed = applyHaEntity(newState, watched_, states_);
                cJSON_Delete(obj);
                return changed;
            }
        }
    }

    cJSON_Delete(obj);
    return false;
}

std::string HaProtocol::buildCallService(const std::string &domain,
                                         const std::string &service,
                                         const std::string &entityId,
                                         const std::string &serviceDataJson) {
    if (!connected_ || !authenticated_) return {};
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddNumberToObject(msg, "id", nextId_++);
    cJSON_AddStringToObject(msg, "type", "call_service");
    cJSON_AddStringToObject(msg, "domain", domain.c_str());
    cJSON_AddStringToObject(msg, "service", service.c_str());
    if (!entityId.empty()) {
        cJSON *target = cJSON_CreateObject();
        cJSON_AddStringToObject(target, "entity_id", entityId.c_str());
        cJSON_AddItemToObject(msg, "target", target);
    }
    if (!serviceDataJson.empty() && serviceDataJson != "{}") {
        if (cJSON *sd = cJSON_Parse(serviceDataJson.c_str())) {
            cJSON_AddItemToObject(msg, "service_data", sd);
        }
    }
    std::string out;
    if (char *printed = cJSON_PrintUnformatted(msg)) {
        out = printed;
        cJSON_free(printed);
    }
    cJSON_Delete(msg);
    return out;
}

void HaProtocol::mergeIntoSnapshot(Snapshot &out) const {
    out.set("ha._connected", connected_);
    for (const auto &[k, v] : states_.entries()) {
        if (v.type == SnapshotValue::Type::Double) out.set(k, v.dVal);
        else if (v.type == SnapshotValue::Type::Bool) out.set(k, v.bVal);
        else if (v.type == SnapshotValue::Type::String) out.set(k, v.sVal);
    }
}

}  // namespace yad
