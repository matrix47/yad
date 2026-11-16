#include "yad/engine.hpp"

#if __has_include(<cjson/cJSON.h>)
#include <cjson/cJSON.h>
#else
#include <cJSON.h>
#endif

#include <algorithm>

namespace yad {

YadEngine::YadEngine(IPlatform &platform, int width, int height, PixelFormat format)
    : platform_(platform), view_(width, height, format) {
    localSys_ = platform_.createLocalSysProvider();
}

YadEngine::~YadEngine() {
    if (haStream_) haStream_->close();
    for (auto &ex : exporters_) {
        if (ex->stream) ex->stream->close();
    }
}

void YadEngine::resize(int width, int height) {
    view_.resize(width, height);
    needRender_ = true;
}

void YadEngine::setPixelFormat(PixelFormat format) {
    view_.setPixelFormat(format);
    needRender_ = true;
}

void YadEngine::nextPage() {
    view_.nextPage();
    needRender_ = true;
}

void YadEngine::prevPage() {
    view_.prevPage();
    needRender_ = true;
}

void YadEngine::setPage(int idx) {
    view_.setPage(idx);
    needRender_ = true;
}

bool YadEngine::loadConfigFromJson(const std::string &jsonText, std::string *error) {
    DashboardConfig cfg;
    if (!DashboardConfig::loadFromJson(jsonText, cfg, error)) return false;
    setConfig(cfg);
    return true;
}

bool YadEngine::loadConfigFromFile(const std::string &path, std::string *error) {
    const std::string txt = platform_.readFileText(path);
    if (txt.empty()) {
        if (error) *error = "Failed to read config: " + path;
        return false;
    }
    return loadConfigFromJson(txt, error);
}

void YadEngine::setConfig(const DashboardConfig &config) {
    config_ = config;
    view_.setConfig(config_);
    setupExporters();
    setupHa();
    needRender_ = true;
}

void YadEngine::setupExporters() {
    for (auto &ex : exporters_) {
        if (ex->stream) ex->stream->close();
    }
    exporters_.clear();

    for (const auto &t : config_.nodeExporters) {
        if (t.urls.empty()) continue;
        auto job = std::make_unique<ExporterJob>();
        job->target = t;
        const std::string pfx = (t.name.empty() || t.name == "default") ? std::string() : (t.name + ".");
        job->parser.setPrefix(pfx);
        job->nextPollMs = 0;
        exporters_.push_back(std::move(job));
    }
}

std::string YadEngine::resolveHaToken(const HomeAssistantConfig &ha) const {
    auto trimCopy = [](std::string s) {
        size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
        size_t b = s.size();
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
        return s.substr(a, b - a);
    };

    if (!ha.token.empty()) return trimCopy(ha.token);
    if (!ha.tokenEnv.empty()) {
        const std::string env = trimCopy(platform_.resolveEnv(ha.tokenEnv));
        if (!env.empty()) return env;
    }
    if (!ha.tokenFile.empty()) {
        std::string path = ha.tokenFile;
        if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
            const std::string home = platform_.resolveEnv("HOME");
            if (!home.empty()) path = home + path.substr(1);
        }
        const std::string raw = trimCopy(platform_.readFileText(path));
        if (!raw.empty()) {
            if (path.size() > 5 && path.substr(path.size() - 5) == ".json") {
                if (cJSON *doc = cJSON_Parse(raw.c_str())) {
                    const cJSON *tk = cJSON_GetObjectItemCaseSensitive(doc, "token");
                    std::string res;
                    if (cJSON_IsString(tk) && tk->valuestring) res = trimCopy(tk->valuestring);
                    cJSON_Delete(doc);
                    if (!res.empty()) return res;
                }
            }
            return raw;
        }
    }
    return {};
}

void YadEngine::setupHa() {
    if (haStream_) {
        haStream_->close();
        haStream_.reset();
    }
    haWs_.reset();
    haTxPending_.clear();
    haEnabled_ = false;
    haStatus_.clear();

    const std::string token = resolveHaToken(config_.homeAssistant);
    haProto_.configure(token, extractWatchedEntities(config_));

    if (config_.homeAssistant.url.empty()) {
        haStatus_ = "no URL";
        return;
    }
    if (token.empty()) {
        haStatus_ = "no token";
        return;
    }

    haUrl_ = ParsedUrl::parse(config_.homeAssistant.url, true);
    if (!haUrl_.valid) {
        haStatus_ = "invalid URL";
        return;
    }
    if (haUrl_.path.empty() || haUrl_.path == "/") {
        haUrl_.path = "/api/websocket";
    }

    haEnabled_ = true;
    haReconnectDelayMs_ = 2000;
    haNextReconnectMs_ = 0;
    haStatus_ = "connecting...";
}

void YadEngine::stepExporters(uint64_t nowMs, bool &dataChanged) {
    for (size_t i = 0; i < exporters_.size(); ++i) {
        ExporterJob &ex = *exporters_[i];
        if (!ex.active) {
            if (nowMs < ex.nextPollMs) continue;
            ex.active = true;
            ex.urlIndex = 0;
            ex.parser.beginPoll();
            ex.stream.reset();
        }

        if (!ex.stream) {
            if (ex.urlIndex >= ex.target.urls.size()) {
                const bool defaultFallback = (!localSys_ && i == 0);
                ex.parser.endPoll(nowMs / 1000.0, snapshot_, defaultFallback);
                ex.active = false;
                ex.nextPollMs = nowMs + static_cast<uint64_t>(std::max(250, ex.target.pollMs));
                dataChanged = true;
                continue;
            }

            ex.currentUrl = ParsedUrl::parse(ex.target.urls[ex.urlIndex], false);
            if (!ex.currentUrl.valid) {
                ++ex.urlIndex;
                continue;
            }
            ex.http.reset();
            ex.txPending = buildHttpGetRequest(ex.currentUrl);
            ex.stream = platform_.createStream();
            if (!ex.stream || !ex.stream->beginConnect(ex.currentUrl.host, ex.currentUrl.port,
                                                       ex.currentUrl.isTls, ex.target.tlsVerify)) {
                ex.stream.reset();
                ++ex.urlIndex;
                continue;
            }
        }

        const auto st = ex.stream->pollConnect();
        if (st == ITcpTlsStream::State::Error || st == ITcpTlsStream::State::Idle) {
            ex.stream.reset();
            ++ex.urlIndex;
            continue;
        }
        if (st != ITcpTlsStream::State::Open) {
            continue;
        }

        if (!ex.txPending.empty()) {
            const int nw = ex.stream->write(reinterpret_cast<const uint8_t *>(ex.txPending.data()), ex.txPending.size());
            if (nw < 0) {
                ex.stream.reset();
                ++ex.urlIndex;
                continue;
            }
            if (nw > 0) ex.txPending.erase(0, static_cast<size_t>(nw));
        }

        if (ex.txPending.empty()) {
            char buf[2048];
            for (int iter = 0; iter < 8; ++iter) {
                const int nr = ex.stream->read(reinterpret_cast<uint8_t *>(buf), sizeof(buf));
                if (nr > 0) {
                    const auto hState = ex.http.feed(buf, static_cast<size_t>(nr), [&](const char *chunk, size_t len) {
                        ex.parser.feed(chunk, len);
                    });
                    if (hState == HttpResponseParser::State::Done || hState == HttpResponseParser::State::Error) {
                        ex.stream.reset();
                        ++ex.urlIndex;
                        break;
                    }
                } else if (nr < 0) {
                    ex.http.finishEof();
                    ex.stream.reset();
                    ++ex.urlIndex;
                    break;
                } else {
                    break;
                }
            }
        }
    }
}

void YadEngine::stepHa(uint64_t nowMs, bool &dataChanged) {
    if (!haEnabled_ || haProto_.authFailed()) return;

    auto scheduleReconnect = [&](const std::string &err) {
        if (haStream_) {
            haStream_->close();
            haStream_.reset();
        }
        haProto_.onDisconnected();
        haWs_.reset();
        haTxPending_.clear();
        haStatus_ = err.empty() ? "disconnected" : err;
        haNextReconnectMs_ = nowMs + haReconnectDelayMs_;
        haReconnectDelayMs_ = std::min<uint32_t>(30000, haReconnectDelayMs_ * 2);
        dataChanged = true;
    };

    if (!haStream_) {
        if (nowMs < haNextReconnectMs_) return;
        haStream_ = platform_.createStream();
        if (!haStream_ || !haStream_->beginConnect(haUrl_.host, haUrl_.port,
                                                   haUrl_.isTls, config_.homeAssistant.tlsVerify)) {
            scheduleReconnect(haStream_ ? haStream_->lastError() : "stream init failed");
            return;
        }
        haWs_.reset();
        uint8_t rnd[16];
        platform_.fillRandom(rnd, sizeof(rnd));
        haTxPending_ = haWs_.buildUpgradeRequest(haUrl_, rnd);
        haProto_.onConnected();
    }

    const auto st = haStream_->pollConnect();
    if (st == ITcpTlsStream::State::Error || st == ITcpTlsStream::State::Idle) {
        scheduleReconnect(haStream_->lastError());
        return;
    }
    if (st != ITcpTlsStream::State::Open) {
        return;
    }

    if (haProto_.isConnected() && nowMs >= haNextPingMs_) {
        uint8_t mask[4];
        platform_.fillRandom(mask, sizeof(mask));
        haTxPending_ += haWs_.encodePingFrame(mask);
        haNextPingMs_ = nowMs + 30000;
    }

    if (!haTxPending_.empty()) {
        const int nw = haStream_->write(reinterpret_cast<const uint8_t *>(haTxPending_.data()), haTxPending_.size());
        if (nw < 0) {
            scheduleReconnect(haStream_->lastError());
            return;
        }
        if (nw > 0) haTxPending_.erase(0, static_cast<size_t>(nw));
    }

    char buf[4096];
    for (int iter = 0; iter < 8; ++iter) {
        const int nr = haStream_->read(reinterpret_cast<uint8_t *>(buf), sizeof(buf));
        if (nr > 0) {
            haWs_.feed(
                buf,
                static_cast<size_t>(nr),
                [&](const std::string &textMsg) {
                    std::vector<std::string> outMsgs;
                    if (haProto_.onTextMessage(textMsg, outMsgs)) {
                        dataChanged = true;
                    }
                    if (haProto_.isConnected()) {
                        haStatus_ = "connected";
                        haReconnectDelayMs_ = 2000;
                        if (haNextPingMs_ == 0) haNextPingMs_ = nowMs + 30000;
                    } else if (haProto_.authFailed()) {
                        haStatus_ = haProto_.lastError();
                    }
                    for (const auto &m : outMsgs) {
                        uint8_t mask[4];
                        platform_.fillRandom(mask, sizeof(mask));
                        haTxPending_ += haWs_.encodeTextFrame(m, mask);
                    }
                },
                haTxPending_);

            if (haWs_.state() == WebSocketFramer::State::Closed || haWs_.state() == WebSocketFramer::State::Error) {
                scheduleReconnect("ws closed");
                return;
            }
            if (haProto_.authFailed()) {
                haStream_->close();
                haStream_.reset();
                dataChanged = true;
                return;
            }
        } else if (nr < 0) {
            scheduleReconnect(haStream_->lastError());
            return;
        } else {
            break;
        }
    }

    if (!haTxPending_.empty()) {
        const int nw = haStream_->write(reinterpret_cast<const uint8_t *>(haTxPending_.data()), haTxPending_.size());
        if (nw > 0) haTxPending_.erase(0, static_cast<size_t>(nw));
    }
}

bool YadEngine::tick(bool forceRender) {
    const uint64_t nowMs = platform_.nowMs();
    bool dataChanged = false;

    stepExporters(nowMs, dataChanged);
    stepHa(nowMs, dataChanged);

    if (localSys_ && (lastLocalSampleMs_ == 0 || (nowMs - lastLocalSampleMs_) >= static_cast<uint64_t>(config_.refreshMs))) {
        localSys_->sample(snapshot_);
        lastLocalSampleMs_ = nowMs;
        dataChanged = true;
    }

    const bool timerDue = (lastRenderMs_ == 0) || ((nowMs - lastRenderMs_) >= static_cast<uint64_t>(config_.refreshMs));
    if (forceRender || needRender_ || dataChanged || timerDue) {
        renderNow();
        return true;
    }
    return false;
}

void YadEngine::renderNow() {
    const uint64_t nowMs = platform_.nowMs();
    haProto_.mergeIntoSnapshot(snapshot_);
    view_.render(snapshot_, nowMs, platform_.nowWallSec());
    lastRenderMs_ = nowMs;
    needRender_ = false;
}

}  // namespace yad
