#include "yad/engine.hpp"
#include "yad/posix_platform.hpp"

#include <SDL3/SDL.h>
#include <png.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

static bool parseSize(const std::string &s, int &w, int &h) {
    const size_t x = s.find_first_of("xX");
    if (x == std::string::npos) return false;
    const int pw = std::atoi(s.substr(0, x).c_str());
    const int ph = std::atoi(s.substr(x + 1).c_str());
    if (pw < 16 || ph < 16) return false;
    w = pw;
    h = ph;
    return true;
}

static bool saveFramebufferPng(const std::string &path, const yad::Framebuffer &fb) {
    FILE *fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        std::fclose(fp);
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        std::fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, static_cast<png_uint_32>(fb.width()), static_cast<png_uint_32>(fb.height()),
                 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<uint8_t> rowRgb(static_cast<size_t>(fb.width()) * 3);
    for (int y = 0; y < fb.height(); ++y) {
        if (fb.format() == yad::PixelFormat::RGB32) {
            const uint32_t *src = fb.data32() + static_cast<size_t>(y) * fb.width();
            for (int x = 0; x < fb.width(); ++x) {
                const uint32_t px = src[x];
                rowRgb[x * 3 + 0] = static_cast<uint8_t>((px >> 16) & 0xFF);
                rowRgb[x * 3 + 1] = static_cast<uint8_t>((px >> 8) & 0xFF);
                rowRgb[x * 3 + 2] = static_cast<uint8_t>(px & 0xFF);
            }
        } else {
            const uint16_t *src = fb.data565() + static_cast<size_t>(y) * fb.width();
            for (int x = 0; x < fb.width(); ++x) {
                const uint16_t px = src[x];
                const uint8_t r5 = static_cast<uint8_t>((px >> 11) & 0x1F);
                const uint8_t g6 = static_cast<uint8_t>((px >> 5) & 0x3F);
                const uint8_t b5 = static_cast<uint8_t>(px & 0x1F);
                rowRgb[x * 3 + 0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
                rowRgb[x * 3 + 1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
                rowRgb[x * 3 + 2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
            }
        }
        png_write_row(png, rowRgb.data());
    }

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(fp);
    return true;
}

static void printUsage(const char *argv0) {
    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "  -c, --config <file>        Path to dashboard JSON config\n"
                 "  -d, --dump <png_file>      Render frame to PNG and exit (headless)\n"
                 "  -s, --size <WxH>           Panel size (default 320x240)\n"
                 "  -f, --format <rgb32|rgb565> Pixel format (default rgb32)\n"
                 "  -n, --node-exporter <url>  Add/override node_exporter URL (repeatable)\n"
                 "      --no-local             Disable local /proc+NVML provider\n"
                 "      --wait-ms <ms>         Wait duration before --dump (default 350)\n"
                 "      --zoom <1..4>          Initial SDL window zoom (default 2)\n",
                 argv0);
}

static bool loadEngineConfig(yad::YadEngine &engine,
                             const std::string &path,
                             const std::vector<std::string> &cliExporters,
                             std::string &err) {
    yad::DashboardConfig cfg;
    if (!yad::DashboardConfig::loadFromFile(path, cfg, &err)) {
        return false;
    }
    if (!cliExporters.empty()) {
        yad::NodeExporterTarget t;
        t.name = "default";
        t.urls = cliExporters;
        t.pollMs = 1000;
        cfg.nodeExporters.clear();
        cfg.nodeExporters.push_back(std::move(t));
    }
    engine.setConfig(cfg);
    return true;
}

}  // namespace

int main(int argc, char *argv[]) {
    std::string configPath;
    std::string dumpPath;
    int width = 320, height = 240;
    yad::PixelFormat format = yad::PixelFormat::RGB32;
    std::vector<std::string> cliExporters;
    bool noLocal = false;
    int waitMs = 350;
    int zoom = 2;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needVal = [&](const char *flag) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing argument for %s\n", flag);
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            configPath = needVal("-c");
        } else if (arg == "-d" || arg == "--dump") {
            dumpPath = needVal("-d");
        } else if (arg == "-s" || arg == "--size") {
            if (!parseSize(needVal("-s"), width, height)) {
                std::fprintf(stderr, "Invalid --size\n");
                return 1;
            }
        } else if (arg == "-f" || arg == "--format") {
            const std::string f = needVal("-f");
            if (f == "rgb32") format = yad::PixelFormat::RGB32;
            else if (f == "rgb565") format = yad::PixelFormat::RGB565;
            else {
                std::fprintf(stderr, "Invalid --format (use rgb32 or rgb565)\n");
                return 1;
            }
        } else if (arg == "-n" || arg == "--node-exporter") {
            cliExporters.emplace_back(needVal("-n"));
        } else if (arg == "--no-local") {
            noLocal = true;
        } else if (arg == "--wait-ms") {
            waitMs = std::max(50, std::atoi(needVal("--wait-ms")));
        } else if (arg == "--zoom") {
            zoom = std::clamp(std::atoi(needVal("--zoom")), 1, 4);
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
            return 1;
        }
    }

    if (configPath.empty()) {
        const char *home = std::getenv("HOME");
        if (home) {
            const std::string userCfg = std::string(home) + "/.config/G19Daemon/dashboard.json";
            if (std::filesystem::exists(userCfg)) configPath = userCfg;
        }
        if (configPath.empty()) configPath = YAD_DEFAULT_CONFIG_PATH;
    }

    yad::PosixPlatform platform(!noLocal);
    yad::YadEngine engine(platform, width, height, format);

    std::string err;
    if (!loadEngineConfig(engine, configPath, cliExporters, err)) {
        std::fprintf(stderr, "Config error: %s\n", err.c_str());
        return 1;
    }

    // Headless PNG dump mode
    if (!dumpPath.empty()) {
        const uint64_t tStart = platform.nowMs();
        engine.getFrame(true);
        while ((platform.nowMs() - tStart) < static_cast<uint64_t>(waitMs)) {
            engine.step();
            usleep(10000);
        }
        // Extra step window if exporters are active so counter deltas settle
        if (engine.exporterCount() > 0) {
            const uint64_t t2 = platform.nowMs();
            while ((platform.nowMs() - t2) < 600) {
                engine.step();
                usleep(10000);
            }
        }
        if (!saveFramebufferPng(dumpPath, engine.getFrame(true))) {
            std::fprintf(stderr, "Failed to save PNG: %s\n", dumpPath.c_str());
            return 1;
        }
        return 0;
    }

    // Interactive SDL3 preview window
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("YAD Preview (zero-Qt)", width * zoom, height * zoom,
                                     SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    auto createTex = [&](yad::PixelFormat fmt) -> SDL_Texture * {
        const SDL_PixelFormat sdlFmt = (fmt == yad::PixelFormat::RGB32)
                                           ? SDL_PIXELFORMAT_ARGB8888
                                           : SDL_PIXELFORMAT_RGB565;
        SDL_Texture *tex = SDL_CreateTexture(renderer, sdlFmt, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        return tex;
    };

    SDL_Texture *texture = createTex(format);

    // Watch config file directory via inotify for live reload
    const std::filesystem::path cfgFsPath = std::filesystem::weakly_canonical(configPath);
    const std::string watchDir = cfgFsPath.parent_path().string();
    const std::string watchName = cfgFsPath.filename().string();
    int inotifyFd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int watchWd = (inotifyFd >= 0 && !watchDir.empty())
                      ? ::inotify_add_watch(inotifyFd, watchDir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO)
                      : -1;

    auto updateTitle = [&]() {
        char title[256];
        const char *fmtName = (format == yad::PixelFormat::RGB32) ? "RGB32" : "RGB565";
        std::snprintf(title, sizeof(title),
                      "YAD [%s %dx%d %dx] | p.%d/%d | HA: %s | [Arrows:Page R:Reload F:Fmt 1-4:Zoom]",
                      fmtName, width, height, zoom,
                      engine.currentPage() + 1, std::max(1, engine.pageCount()),
                      engine.haError().empty() ? (engine.haConnected() ? "ok" : "off") : engine.haError().c_str());
        SDL_SetWindowTitle(window, title);
    };

    engine.getFrame(true);
    engine.step();
    updateTitle();
    uint64_t lastTitleMs = platform.nowMs();
    uint64_t nextRenderMs = lastTitleMs + static_cast<uint64_t>(engine.getIntervalMs());

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode key = ev.key.key;
                if (key == SDLK_ESCAPE || key == SDLK_Q) {
                    running = false;
                } else if (key == SDLK_RIGHT) {
                    engine.nextPage();
                    engine.getFrame(false);
                    engine.step();
                    nextRenderMs = platform.nowMs() + static_cast<uint64_t>(engine.getIntervalMs());
                    updateTitle();
                } else if (key == SDLK_LEFT) {
                    engine.prevPage();
                    engine.getFrame(false);
                    engine.step();
                    nextRenderMs = platform.nowMs() + static_cast<uint64_t>(engine.getIntervalMs());
                    updateTitle();
                } else if (key == SDLK_R) {
                    if (loadEngineConfig(engine, configPath, cliExporters, err)) {
                        engine.getFrame(true);
                        engine.step();
                        nextRenderMs = platform.nowMs() + static_cast<uint64_t>(engine.getIntervalMs());
                        updateTitle();
                    }
                } else if (key == SDLK_F) {
                    format = (format == yad::PixelFormat::RGB32) ? yad::PixelFormat::RGB565 : yad::PixelFormat::RGB32;
                    engine.setPixelFormat(format);
                    engine.getFrame(false);
                    SDL_DestroyTexture(texture);
                    texture = createTex(format);
                    updateTitle();
                } else if (key >= SDLK_1 && key <= SDLK_4) {
                    zoom = static_cast<int>(key - SDLK_0);
                    SDL_SetWindowSize(window, width * zoom, height * zoom);
                    updateTitle();
                }
            }
        }

        if (inotifyFd >= 0 && watchWd >= 0) {
            char ibuf[1024];
            ssize_t len = ::read(inotifyFd, ibuf, sizeof(ibuf));
            bool reload = false;
            for (ssize_t off = 0; len > 0 && off < len;) {
                auto *iev = reinterpret_cast<struct inotify_event *>(ibuf + off);
                if (iev->len > 0 && watchName == iev->name) reload = true;
                off += static_cast<ssize_t>(sizeof(struct inotify_event)) + iev->len;
            }
            if (reload && loadEngineConfig(engine, configPath, cliExporters, err)) {
                engine.getFrame(true);
                engine.step();
                nextRenderMs = platform.nowMs() + static_cast<uint64_t>(engine.getIntervalMs());
                updateTitle();
            }
        }

        engine.step();
        const uint64_t now = platform.nowMs();
        const uint64_t interval = static_cast<uint64_t>(engine.getIntervalMs());
        if (now >= nextRenderMs) {
            engine.getFrame(true);
            nextRenderMs += interval;
            if (now >= nextRenderMs + interval) {
                nextRenderMs = now + interval;
            }
        }
        if (now - lastTitleMs >= 500) {
            updateTitle();
            lastTitleMs = now;
        }

        if (texture) {
            const auto &fb = engine.framebuffer();
            SDL_UpdateTexture(texture, nullptr, fb.rawData(), fb.strideBytes());
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }

        SDL_Delay(16);
    }

    if (inotifyFd >= 0) ::close(inotifyFd);
    if (texture) SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
