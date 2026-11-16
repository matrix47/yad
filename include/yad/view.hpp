#pragma once

#include "yad/config.hpp"
#include "yad/fb.hpp"
#include "yad/types.hpp"
#include <ctime>
#include <memory>
#include <vector>

namespace yad {

class DashboardView {
public:
    DashboardView(int width = 320, int height = 240, PixelFormat format = PixelFormat::RGB32);
    ~DashboardView();

    void resize(int width, int height);
    void setPixelFormat(PixelFormat format);
    PixelFormat pixelFormat() const { return fb_.format(); }
    int width() const { return fb_.width(); }
    int height() const { return fb_.height(); }

    void setConfig(const DashboardConfig &config);
    const DashboardConfig &config() const { return config_; }

    int pageCount() const { return static_cast<int>(config_.pages.size()); }
    int currentPage() const { return currentPage_; }
    void setPage(int index);
    void nextPage();
    void prevPage();

    void render(const Snapshot &snapshot, uint64_t nowMs, std::time_t nowWallSec = 0);

    const Framebuffer &framebuffer() const { return fb_; }
    const uint32_t *pixels32() const { return fb_.data32(); }
    const uint16_t *pixels565() const { return fb_.data565(); }

private:
    struct CardState;
    void rebuildPage();

    DashboardConfig config_;
    int currentPage_ = 0;
    Framebuffer fb_;
    std::vector<std::unique_ptr<CardState>> cards_;
};

}  // namespace yad
