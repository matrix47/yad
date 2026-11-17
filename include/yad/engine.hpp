#pragma once

#include "yad/config.hpp"
#include "yad/net_proto.hpp"
#include "yad/platform.hpp"
#include "yad/providers.hpp"
#include "yad/view.hpp"
#include <memory>
#include <string>
#include <vector>

namespace yad {

class YadEngine {
public:
    YadEngine(IPlatform &platform,
              int width = 320,
              int height = 240,
              PixelFormat format = PixelFormat::RGB32);
    ~YadEngine();

    bool loadConfigFromJson(const std::string &jsonText, std::string *error = nullptr);
    bool loadConfigFromFile(const std::string &path, std::string *error = nullptr);
    void setConfig(const DashboardConfig &config);
    const DashboardConfig &config() const { return config_; }

    void resize(int width, int height);
    void setPixelFormat(PixelFormat format);
    void nextPage();
    void prevPage();
    void setPage(int idx);
    int pageCount() const { return view_.pageCount(); }
    int currentPage() const { return view_.currentPage(); }

    // Configured page refresh interval in milliseconds (default 1000, min 100).
    int getIntervalMs() const;

    // Non-blocking step: polls Home Assistant WebSocket updates and advances any
    // active/needed node_exporter HTTP fetches into snapshot_.
    void step();

    // Renders and returns the current framebuffer:
    // - sampleSensors = true (periodic interval pull): samples local sensors and
    //   marks active node_exporters as needed when their poll_ms period is due.
    // - sampleSensors = false (e.g. page navigation): renders current snapshot immediately.
    const Framebuffer &getFrame(bool sampleSensors = true);

    const Framebuffer &framebuffer() const { return view_.framebuffer(); }
    const uint32_t *pixels32() const { return view_.pixels32(); }
    const uint16_t *pixels565() const { return view_.pixels565(); }
    int width() const { return view_.width(); }
    int height() const { return view_.height(); }

    bool haConnected() const { return haProto_.isConnected(); }
    const std::string &haError() const { return haStatus_; }
    size_t exporterCount() const { return exporters_.size(); }
    bool hasLocalProvider() const { return localSys_ != nullptr; }

private:
    struct ExporterJob {
        NodeExporterTarget target;
        std::vector<std::string> filteredUrls;
        NodeExporterParser parser;
        std::unique_ptr<ITcpTlsStream> stream;
        HttpResponseParser http;
        ParsedUrl currentUrl;
        size_t urlIndex = 0;
        std::string txPending;
        uint64_t nextRetryMs = 0;
        int framesUntilFetch = 1;
        bool needed = true;
        bool active = false;
        bool anySuccess = false;
        bool useFiltered = true;
        bool usedOnCurrentPage = true;
        bool hasWarmedDelta = false;
    };

    void setupExporters();
    void updateExporterUsageForCurrentPage();
    void setupHa();
    void stepExporters(uint64_t nowMs);
    void stepHa(uint64_t nowMs);
    std::string resolveHaToken(const HomeAssistantConfig &ha) const;

    IPlatform &platform_;
    DashboardConfig config_;
    DashboardView view_;
    Snapshot snapshot_;
    std::unique_ptr<ILocalSysProvider> localSys_;

    std::vector<std::unique_ptr<ExporterJob>> exporters_;

    // Home Assistant WebSocket state
    HaProtocol haProto_;
    ParsedUrl haUrl_;
    std::unique_ptr<ITcpTlsStream> haStream_;
    WebSocketFramer haWs_;
    std::string haTxPending_;
    std::string haStatus_;
    bool haEnabled_ = false;
    uint64_t haNextReconnectMs_ = 0;
    uint64_t haNextPingMs_ = 0;
    uint32_t haReconnectDelayMs_ = 2000;

    bool hasLocalSample_ = false;
};

}  // namespace yad
