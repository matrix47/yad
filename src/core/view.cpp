#include "yad/view.hpp"
#include "yad/assets.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace yad {

namespace {

static std::string toLower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string trim(const std::string &s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static std::string resolveMetricKey(const ParamMap &p, const std::string &rawKey) {
    if (rawKey.empty()) return {};
    std::string host = p.contains("node_exporter") ? p.getString("node_exporter") : p.getString("source_host");
    host = trim(host);
    if (host.empty() || toLower(host) == "local") {
        return rawKey;
    }
    const std::string prefix = host + ".";
    if (rawKey.compare(0, prefix.size(), prefix) == 0) {
        return rawKey;
    }
    return prefix + rawKey;
}

static Color lerpColor(Color a, Color b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return Color::rgba(
        static_cast<uint8_t>(std::llround(a.r + (b.r - a.r) * t)),
        static_cast<uint8_t>(std::llround(a.g + (b.g - a.g) * t)),
        static_cast<uint8_t>(std::llround(a.b + (b.b - a.b) * t)));
}

static Color btopThermalColor(double pct) {
    const Color green = Color::rgba(0x34, 0xc7, 0x59);
    const Color yellow = Color::rgba(0xf5, 0xc4, 0x51);
    const Color orange = Color::rgba(0xff, 0x95, 0x00);
    const Color red = Color::rgba(0xff, 0x3b, 0x30);
    pct = std::clamp(pct, 0.0, 100.0);
    if (pct < 50.0) return lerpColor(green, yellow, pct / 50.0);
    if (pct < 80.0) return lerpColor(yellow, orange, (pct - 50.0) / 30.0);
    return lerpColor(orange, red, (pct - 80.0) / 20.0);
}

static std::string formatRate(double bps) {
    char buf[32];
    if (bps >= 1024.0 * 1024.0 * 1024.0) std::snprintf(buf, sizeof(buf), "%.1fG", bps / (1024.0 * 1024.0 * 1024.0));
    else if (bps >= 1024.0 * 1024.0) std::snprintf(buf, sizeof(buf), "%.1fM", bps / (1024.0 * 1024.0));
    else if (bps >= 1024.0) std::snprintf(buf, sizeof(buf), "%.0fK", bps / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%dB", static_cast<int>(std::llround(bps)));
    return buf;
}

static std::string formatQtDateTime(const std::string &fmt, const std::tm &tmVal) {
    static const char *kDaysShort[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *kMonthsShort[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    std::string out;
    out.reserve(fmt.size() + 8);
    size_t i = 0;
    while (i < fmt.size()) {
        auto matchAt = [&](const char *tok) {
            const size_t len = std::strlen(tok);
            if (fmt.compare(i, len, tok) == 0) {
                i += len;
                return true;
            }
            return false;
        };
        char buf[16];
        if (matchAt("yyyy")) { std::snprintf(buf, sizeof(buf), "%04d", tmVal.tm_year + 1900); out += buf; }
        else if (matchAt("yy")) { std::snprintf(buf, sizeof(buf), "%02d", (tmVal.tm_year + 1900) % 100); out += buf; }
        else if (matchAt("MMM")) { out += kMonthsShort[std::clamp(tmVal.tm_mon, 0, 11)]; }
        else if (matchAt("MM")) { std::snprintf(buf, sizeof(buf), "%02d", tmVal.tm_mon + 1); out += buf; }
        else if (matchAt("ddd")) { out += kDaysShort[std::clamp(tmVal.tm_wday, 0, 6)]; }
        else if (matchAt("dd")) { std::snprintf(buf, sizeof(buf), "%02d", tmVal.tm_mday); out += buf; }
        else if (matchAt("HH")) { std::snprintf(buf, sizeof(buf), "%02d", tmVal.tm_hour); out += buf; }
        else if (matchAt("mm")) { std::snprintf(buf, sizeof(buf), "%02d", tmVal.tm_min); out += buf; }
        else if (matchAt("ss")) { std::snprintf(buf, sizeof(buf), "%02d", tmVal.tm_sec); out += buf; }
        else { out.push_back(fmt[i++]); }
    }
    return out;
}

struct WeatherVisual {
    const char *icon;
    Color tint;
};

static WeatherVisual weatherConditionVisual(const std::string &cond, Color fallbackTint) {
    const std::string c = toLower(trim(cond));
    if (c == "sunny") return {"weather_sunny", Color::rgba(0xf5, 0xc4, 0x51)};
    if (c == "clear-night") return {"weather_night", Color::rgba(0xa8, 0xc7, 0xfa)};
    if (c == "partlycloudy") return {"weather_partlycloudy", Color::rgba(0xf5, 0xc4, 0x51)};
    if (c == "cloudy") return {"weather_cloudy", Color::rgba(0x9f, 0xb3, 0xc8)};
    if (c == "rainy" || c == "pouring") return {"weather_rainy", Color::rgba(0x00, 0xa5, 0xdb)};
    if (c.find("lightning") != std::string::npos) return {"weather_lightning", Color::rgba(0xff, 0xd1, 0x66)};
    if (c.find("snow") != std::string::npos || c == "hail") return {"weather_snowy", Color::rgba(0xdc, 0xee, 0xff)};
    if (c == "fog") return {"weather_fog", Color::rgba(0x9f, 0xb3, 0xc8)};
    if (c.find("wind") != std::string::npos) return {"weather_windy", Color::rgba(0x64, 0xd2, 0xff)};
    return {"weather", fallbackTint};
}

static int resolvePixels(const SizeSpec &spec, int availableTotal) {
    switch (spec.mode) {
        case SizeSpec::Mode::FixedPx: return std::max(1, static_cast<int>(std::llround(spec.value)));
        case SizeSpec::Mode::Percent: return std::max(1, static_cast<int>(std::llround((availableTotal * spec.value) / 100.0)));
        case SizeSpec::Mode::Stretch:
        default: return 0;
    }
}

static std::vector<int> distributeSizes(const std::vector<SizeSpec> &specs, int totalAvail, int gap) {
    const int n = static_cast<int>(specs.size());
    std::vector<int> out(n, 0);
    const int usable = std::max(0, totalAvail - std::max(0, n - 1) * gap);

    int fixedUsed = 0;
    double stretchSum = 0.0;
    for (int i = 0; i < n; ++i) {
        if (specs[i].mode == SizeSpec::Mode::Stretch) stretchSum += std::max(1.0, specs[i].value);
        else {
            out[i] = resolvePixels(specs[i], usable);
            fixedUsed += out[i];
        }
    }
    if (stretchSum > 0.0) {
        const int rem = std::max(0, usable - fixedUsed);
        int assigned = 0;
        int lastIdx = -1;
        for (int i = 0; i < n; ++i) {
            if (specs[i].mode == SizeSpec::Mode::Stretch) {
                lastIdx = i;
                const int px = static_cast<int>(std::floor((rem * std::max(1.0, specs[i].value)) / stretchSum));
                out[i] = px;
                assigned += px;
            }
        }
        if (lastIdx >= 0 && assigned < rem) out[lastIdx] += (rem - assigned);
    }
    return out;
}

static void drawIconTextRow(Framebuffer &fb,
                            const Rect &r,
                            const IconAsset *icon,
                            Color iconTint,
                            const BitmapFont *font,
                            const std::string &text,
                            Color textColor,
                            int spacing) {
    if (!font || r.empty()) return;
    const int textW = fb.measureText(*font, text);
    const int iconW = icon ? (icon->size + spacing) : 0;
    const int totalW = iconW + textW;
    int x = r.x;
    if (totalW < r.w) x += (r.w - totalW) / 2;
    if (icon) {
        const int iy = r.y + (r.h - icon->size) / 2;
        fb.drawIcon(x, iy, *icon, iconTint);
        x += icon->size + spacing;
    }
    const Rect textRect{x, r.y, std::max(0, r.x + r.w - x), r.h};
    fb.setClip(textRect);
    fb.drawText(textRect, *font, text, textColor, AlignLeft | AlignVCenter);
}

static bool consumeMetricSample(const Snapshot &snapshot,
                                const ParamMap &p,
                                const std::string &presenceKey,
                                bool &hasSeq,
                                double &lastSeq) {
    const std::string seqKey = resolveMetricKey(p, "_seq");
    if (snapshot.contains(seqKey)) {
        const double seq = snapshot.getDouble(seqKey, -1.0);
        if (hasSeq && seq == lastSeq) return false;
        lastSeq = seq;
        hasSeq = true;
        return true;
    }
    return presenceKey.empty() || snapshot.contains(presenceKey);
}

}  // namespace

struct DashboardView::CardState {
    enum class Kind {
        Spacer,
        Clock,
        Weather,
        BtopBar,
        NetActivity,
        SensorDot,
        HaSensor,
        Placeholder
    };

    Kind kind = Kind::Spacer;
    Rect cellRect;
    Rect frameRect;
    Rect contentRect;
    const NinePatchAsset *ninePatch = nullptr;
    Color background = Color::invalid();
    ColumnConfig col;

    std::vector<double> history;
    double autoPeakBps = 128.0 * 1024.0;
    double lastSeq = -1.0;
    bool hasSeq = false;
    int scrollOffset = 0;
    int scrollDir = 1;
    uint64_t lastThrottleMs = 0;
    bool hasThrottle = false;
    std::string cachedText;
    std::string cachedIcon = "weather";
    Color cachedTint = Color::invalid();
};

DashboardView::DashboardView(int width, int height, PixelFormat format) : fb_(width, height, format) {}
DashboardView::~DashboardView() = default;

void DashboardView::setPixelFormat(PixelFormat format) { fb_.setFormat(format); }

void DashboardView::resize(int width, int height) {
    if (width != fb_.width() || height != fb_.height()) {
        fb_.resize(width, height);
        rebuildPage();
    }
}

void DashboardView::setConfig(const DashboardConfig &config) {
    config_ = config;
    if (currentPage_ >= static_cast<int>(config_.pages.size())) currentPage_ = 0;
    rebuildPage();
}

void DashboardView::setPage(int index) {
    if (config_.pages.empty()) return;
    const int n = static_cast<int>(config_.pages.size());
    const int wrapped = ((index % n) + n) % n;
    if (wrapped != currentPage_) {
        currentPage_ = wrapped;
        rebuildPage();
    }
}

void DashboardView::nextPage() { setPage(currentPage_ + 1); }
void DashboardView::prevPage() { setPage(currentPage_ - 1); }

void DashboardView::rebuildPage() {
    cards_.clear();
    if (config_.pages.empty()) return;

    const PageConfig &page = config_.pages[currentPage_];
    const int margin = config_.theme.outerMargin;
    const int rowGap = config_.theme.rowGap;
    const int rootW = std::max(1, fb_.width() - 2 * margin);
    const int rootH = std::max(1, fb_.height() - 2 * margin);

    std::vector<SizeSpec> rowSpecs;
    rowSpecs.reserve(page.rows.size());
    for (const auto &row : page.rows) rowSpecs.push_back(row.height);
    const auto rowHeights = distributeSizes(rowSpecs, rootH, rowGap);

    int curY = margin;
    for (size_t ri = 0; ri < page.rows.size(); ++ri) {
        const RowConfig &row = page.rows[ri];
        const int rowH = rowHeights[ri];

        std::vector<SizeSpec> colSpecs;
        colSpecs.reserve(row.columns.size());
        for (const auto &col : row.columns) colSpecs.push_back(col.width);
        const auto colWidths = distributeSizes(colSpecs, rootW, row.gap);

        int curX = margin;
        for (size_t ci = 0; ci < row.columns.size(); ++ci) {
            const ColumnConfig &col = row.columns[ci];
            const int cellW = colWidths[ci];

            auto st = std::make_unique<CardState>();
            st->col = col;
            st->cellRect = {curX, curY, cellW, rowH};
            st->background = col.background;

            std::string npName = col.ninePatch.empty() ? config_.theme.defaultNinePatch : col.ninePatch;
            if (!npName.empty() && toLower(npName) != "none") {
                st->ninePatch = findNinePatchAsset(npName);
            }

            int fw = cellW, fh = rowH;
            if (col.cardWidth.value > 0) fw = std::min(cellW, resolvePixels(col.cardWidth, rootW));
            if (col.cardHeight.value > 0) fh = std::min(rowH, resolvePixels(col.cardHeight, rootH));
            int fx = curX, fy = curY;
            if ((col.cardWidth.value > 0 || col.cardHeight.value > 0) && col.hasAlignment) {
                if (col.alignment & AlignRight) fx = curX + (cellW - fw);
                else if (col.alignment & AlignHCenter) fx = curX + (cellW - fw) / 2;
                if (col.alignment & AlignBottom) fy = curY + (rowH - fh);
                else if (col.alignment & AlignVCenter) fy = curY + (rowH - fh) / 2;
            }
            st->frameRect = {fx, fy, fw, fh};

            Margins m = col.padding;
            if (st->ninePatch) m = st->ninePatch->contentMargins + m;
            st->contentRect = {
                fx + m.left,
                fy + m.top,
                std::max(0, fw - m.left - m.right),
                std::max(0, fh - m.top - m.bottom)
            };

            const std::string t = toLower(col.cardType);
            if (t == "clock") st->kind = CardState::Kind::Clock;
            else if (t == "weather") st->kind = CardState::Kind::Weather;
            else if (t == "btop_bar") st->kind = CardState::Kind::BtopBar;
            else if (t == "net_activity") st->kind = CardState::Kind::NetActivity;
            else if (t == "ha_binary" || t == "sensor_dot") st->kind = CardState::Kind::SensorDot;
            else if (t == "ha_sensor") st->kind = CardState::Kind::HaSensor;
            else if (t == "spacer") st->kind = CardState::Kind::Spacer;
            else st->kind = CardState::Kind::Placeholder;

            cards_.push_back(std::move(st));
            curX += cellW + row.gap;
        }
        curY += rowH + rowGap;
    }
}

void DashboardView::render(const Snapshot &snapshot, uint64_t nowMs, std::time_t nowWallSec) {
    fb_.resetClip();
    fb_.clear(config_.theme.background);

    if (nowWallSec == 0) nowWallSec = std::time(nullptr);
    std::tm tmLocal{};
    localtime_r(&nowWallSec, &tmLocal);

    const ThemeConfig &theme = config_.theme;

    for (auto &stPtr : cards_) {
        CardState &st = *stPtr;
        const Rect &fr = st.frameRect;
        const Rect &r = st.contentRect;

        fb_.setClip(fr);
        if (st.background.valid) fb_.fillRect(fr, st.background);
        if (st.ninePatch) fb_.drawNinePatch(fr, *st.ninePatch);
        if (r.empty() || st.kind == CardState::Kind::Spacer) continue;

        fb_.setClip(r);
        const ParamMap &p = st.col.cardParams;

        switch (st.kind) {
            case CardState::Kind::Clock: {
                const std::string timeFmt = p.contains("time_format") ? p.getString("time_format") : "HH:mm:ss";
                const std::string dateFmt = p.contains("date_format") ? p.getString("date_format") : "ddd dd MMM";
                const bool showIcon = p.getBool("show_icon", true);
                const std::string iconName = p.getString("icon", "clock");
                const int iconSize = p.getInt("icon_size", 14);
                const int timeSize = p.getInt("time_size", std::max(16, theme.fontSize));
                const int dateSize = p.getInt("date_size", theme.fontSize);
                const Color timeColor = p.contains("color") ? Color::fromHex(p.getString("color"), theme.foreground) : theme.foreground;
                const Color dateColor = p.contains("date_color") ? Color::fromHex(p.getString("date_color"), theme.muted) : theme.muted;
                const uint8_t align = p.contains("text_alignment") ? parseAlignment(p.getString("text_alignment")) : (AlignLeft | AlignVCenter);

                int x = r.x;
                if (showIcon && !iconName.empty()) {
                    if (const IconAsset *ic = findIconAsset(iconName, iconSize)) {
                        fb_.drawIcon(x, r.y + (r.h - ic->size) / 2, *ic, theme.accent);
                        x += ic->size + 4;
                    }
                }
                const Rect textBox{x, r.y, std::max(0, r.x + r.w - x), r.h};
                const BitmapFont *tf = !timeFmt.empty() ? findBitmapFont(timeSize, true) : nullptr;
                const BitmapFont *df = !dateFmt.empty() ? findBitmapFont(dateSize, false) : nullptr;
                if (tf && df) {
                    const int totalH = tf->lineHeight + df->lineHeight;
                    const int y0 = textBox.y + (textBox.h - totalH) / 2;
                    fb_.drawText({textBox.x, y0, textBox.w, tf->lineHeight}, *tf, formatQtDateTime(timeFmt, tmLocal), timeColor, align);
                    fb_.drawText({textBox.x, y0 + tf->lineHeight, textBox.w, df->lineHeight}, *df, formatQtDateTime(dateFmt, tmLocal), dateColor, align);
                } else if (tf) {
                    fb_.drawText(textBox, *tf, formatQtDateTime(timeFmt, tmLocal), timeColor, align);
                } else if (df) {
                    fb_.drawText(textBox, *df, formatQtDateTime(dateFmt, tmLocal), dateColor, align);
                }
                break;
            }

            case CardState::Kind::Weather: {
                const std::string entity = p.getString("entity", "weather.home");
                const std::string condEntity = p.getString("condition_entity");
                const std::string unit = p.getString("unit", "°C");
                const int precision = p.getInt("precision", 1);
                const int fontSize = p.getInt("font_size", theme.fontSize);
                const bool bold = p.getBool("bold", false);
                const int iconSize = p.getInt("icon_size", fontSize);
                const int spacing = p.getInt("spacing", 3);
                const uint64_t minUpdateMs = static_cast<uint64_t>(std::max(0, p.getInt("min_update_ms", 10000)));

                if (!st.hasThrottle || minUpdateMs == 0 || (nowMs - st.lastThrottleMs) >= minUpdateMs) {
                    std::string text = "--" + unit;
                    std::string cond;
                    const std::string stateKey = "ha." + entity;
                    const std::string tempAttrKey = stateKey + ".temperature";

                    bool hasEntityData = false;
                    if (snapshot.contains(tempAttrKey)) {
                        hasEntityData = true;
                        bool ok = false;
                        double temp = snapshot.getDouble(tempAttrKey, 0.0, &ok);
                        if (ok) {
                            if (snapshot.getString(stateKey + ".temperature_unit") == "°F" && unit == "°C") {
                                temp = (temp - 32.0) * 5.0 / 9.0;
                            }
                            char buf[32];
                            std::snprintf(buf, sizeof(buf), "%.*f%s", precision, temp, unit.c_str());
                            text = buf;
                        }
                        cond = snapshot.getString(stateKey);
                    } else if (snapshot.contains(stateKey)) {
                        hasEntityData = true;
                        bool ok = false;
                        double val = snapshot.getDouble(stateKey, 0.0, &ok);
                        if (ok) {
                            if (snapshot.getString(stateKey + ".unit_of_measurement") == "°F" && unit == "°C") {
                                val = (val - 32.0) * 5.0 / 9.0;
                            }
                            char buf[32];
                            std::snprintf(buf, sizeof(buf), "%.*f%s", precision, val, unit.c_str());
                            text = buf;
                        }
                    }
                    if (!condEntity.empty()) cond = snapshot.getString("ha." + condEntity);
                    const WeatherVisual vis = weatherConditionVisual(cond, theme.accent);
                    st.cachedText = text;
                    st.cachedIcon = vis.icon;
                    st.cachedTint = vis.tint;
                    if (hasEntityData) {
                        st.lastThrottleMs = nowMs;
                        st.hasThrottle = true;
                    }
                }
                const IconAsset *ic = findIconAsset(st.cachedIcon, iconSize);
                const BitmapFont *fnt = findBitmapFont(fontSize, bold);
                drawIconTextRow(fb_, r, ic, st.cachedTint.valid ? st.cachedTint : theme.accent,
                                fnt, st.cachedText.empty() ? ("--" + unit) : st.cachedText, theme.foreground, spacing);
                break;
            }

            case CardState::Kind::BtopBar: {
                if (r.w < 16 || r.h < 14) break;
                const std::string source = resolveMetricKey(p, p.getString("source"));
                const std::string detailSource = resolveMetricKey(p, p.getString("detail_source"));
                const std::string label = p.getString("label", p.getString("source"));
                const std::string iconName = p.getString("icon");
                const std::string unit = p.getString("unit", "%");
                const double maxVal = std::max(0.001, p.getDouble("max_value", 100.0));
                const bool showGraph = p.getBool("show_graph", true);
                const bool showBar = p.getBool("show_bar", true);
                const int fontSize = std::max(6, p.getInt("font_size", theme.fontSize));
                const Color fixedColor = p.contains("color") ? Color::fromHex(p.getString("color")) : Color::invalid();

                const double raw = snapshot.getDouble(source, 0.0);
                const double pct = std::clamp((raw / maxVal) * 100.0, 0.0, 100.0);
                std::string detail;
                if (!detailSource.empty() && snapshot.contains(detailSource)) {
                    detail = snapshot.getString(detailSource);
                } else if (unit == "%") {
                    detail = std::to_string(static_cast<int>(std::llround(pct))) + "%";
                } else {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.1f%s", raw, unit.c_str());
                    detail = buf;
                }

                if (consumeMetricSample(snapshot, p, source, st.hasSeq, st.lastSeq)) {
                    st.history.push_back(pct);
                    if (st.history.size() > 64) st.history.erase(st.history.begin());
                }

                const int headerH = std::max(14, fontSize + 2);
                const int barH = showBar ? 7 : 0;
                const int gap = 2;

                int x = r.x;
                if (!iconName.empty()) {
                    if (const IconAsset *ic = findIconAsset(iconName, 12)) {
                        fb_.drawIcon(x, r.y + (headerH - ic->size) / 2, *ic, theme.accent);
                        x += ic->size + 4;
                    }
                }

                const BitmapFont *labelFont = findBitmapFont(fontSize, true);
                const BitmapFont *detailFont = findBitmapFont(fontSize, false);
                const int detailW = detailFont ? fb_.measureText(*detailFont, detail) : 0;

                if (detailFont) {
                    fb_.drawText({r.right() - detailW, r.y, detailW + 1, headerH},
                                 *detailFont, detail, btopThermalColor(pct), AlignRight | AlignVCenter);
                }
                if (labelFont) {
                    const int availLabelW = std::max(0, r.right() - detailW - 4 - x);
                    const Rect labelRect{x, r.y, availLabelW, headerH};
                    fb_.setClip(labelRect);
                    fb_.drawText(labelRect, *labelFont, label, theme.foreground, AlignLeft | AlignVCenter);
                    fb_.setClip(r);
                }

                int yTop = r.y + headerH + gap;
                const int graphH = r.bottom() - yTop - (showBar ? (barH + gap) : 0);
                if (showGraph && graphH >= 6) {
                    const Rect graphRect{r.x, yTop, r.w, graphH};
                    fb_.fillRect(graphRect, Color::rgba(10, 15, 22, 160));

                    const int colW = 2, colStep = 3;
                    const int maxCols = std::max(1, graphRect.w / colStep);
                    const int histSize = static_cast<int>(st.history.size());
                    const int startIdx = std::max(0, histSize - maxCols);

                    int gx = graphRect.right() - ((histSize - startIdx) * colStep) + 1;
                    if (gx < graphRect.x) gx = graphRect.x;
                    for (int i = startIdx; i < histSize; ++i) {
                        const double val = st.history[i];
                        int h = static_cast<int>(std::llround((val / 100.0) * graphRect.h));
                        if (val > 0.5 && h == 0) h = 1;
                        if (h > 0) {
                            const Color c = fixedColor.valid ? fixedColor : btopThermalColor(val);
                            fb_.fillRect({gx, graphRect.bottom() - h + 1, colW, h}, c);
                        }
                        gx += colStep;
                    }
                }

                if (showBar) {
                    const int barY = r.bottom() - barH + 1;
                    const int segW = 3, segStep = 4;
                    const int segCount = std::max(1, r.w / segStep);
                    const int litCount = static_cast<int>(std::llround((pct / 100.0) * segCount));
                    const Color track = Color::rgba(30, 41, 54);
                    for (int i = 0; i < segCount; ++i) {
                        const int sx = r.x + i * segStep;
                        const double segPct = ((i + 1) * 100.0) / segCount;
                        const Color c = (i < litCount)
                                            ? (fixedColor.valid ? fixedColor : btopThermalColor(segPct))
                                            : track;
                        fb_.fillRect({sx, barY, segW, barH}, c);
                    }
                }
                break;
            }

            case CardState::Kind::NetActivity: {
                if (r.w < 40 || r.h < 24) break;
                const std::string iface = p.getString("iface", "eth0");
                const std::string label = p.getString("label", iface);
                const std::string iconName = p.getString("icon", "lan");
                const int fontSize = std::max(6, p.getInt("font_size", theme.fontSize));
                const double maxMbps = p.getDouble("max_mbps", 0.0);
                const double maxBps = (maxMbps > 0.0) ? ((maxMbps * 1000.0 * 1000.0) / 8.0) : 0.0;

                const std::string prefix = resolveMetricKey(p, "net." + iface + ".");
                const bool isUp = snapshot.getBool(prefix + "up", false);
                const double rxBps = snapshot.getDouble(prefix + "rx_bps", 0.0);
                const double txBps = snapshot.getDouble(prefix + "tx_bps", 0.0);
                if (maxBps <= 0.0 && consumeMetricSample(snapshot, p, prefix + "rx_bps", st.hasSeq, st.lastSeq)) {
                    const double curMax = std::max(rxBps, txBps);
                    st.autoPeakBps = std::max(128.0 * 1024.0, std::max(curMax, st.autoPeakBps * 0.92));
                }

                const int headerH = std::max(14, fontSize + 2);
                int x = r.x;
                if (!iconName.empty()) {
                    if (const IconAsset *ic = findIconAsset(iconName, 12)) {
                        fb_.drawIcon(x, r.y + (headerH - ic->size) / 2, *ic, theme.accent);
                        x += ic->size + 4;
                    }
                }
                if (const BitmapFont *lf = findBitmapFont(fontSize, true)) {
                    fb_.drawText({x, r.y, std::max(0, r.w - (x - r.x) - 12), headerH},
                                 *lf, label, theme.foreground, AlignLeft | AlignVCenter);
                }

                const Color stateColor = isUp ? Color::rgba(0x34, 0xc7, 0x59) : Color::rgba(0xff, 0x3b, 0x30);
                fb_.fillCircle(static_cast<float>(r.right() - 4),
                               static_cast<float>(r.y) + headerH / 2.0f,
                               3.0f, stateColor);

                const double scaleMax = (maxBps > 0.0) ? maxBps : st.autoPeakBps;
                const double rxRatio = std::clamp(rxBps / scaleMax, 0.0, 1.0);
                const double txRatio = std::clamp(txBps / scaleMax, 0.0, 1.0);

                const int bodyY = r.y + headerH + 2;
                const int rowH = std::max(10, (r.bottom() - bodyY + 1) / 2);
                const BitmapFont *rf = findBitmapFont(fontSize, false);

                auto drawSegRow = [&](const Rect &rowRect, const std::string &glyph, const std::string &rateText, double ratio, Color barCol) {
                    if (!rf) return;
                    const int prefixW = std::max(10, fb_.measureText(*rf, glyph) + 2);
                    const int rateW = std::max(40, fb_.measureText(*rf, rateText) + 2);
                    const int barX = rowRect.x + prefixW;
                    const int barW = std::max(10, rowRect.w - prefixW - rateW - 2);
                    const int barH = 6;
                    const int barY = rowRect.y + (rowRect.h - barH) / 2;

                    fb_.drawText({rowRect.x, rowRect.y, prefixW, rowRect.h}, *rf, glyph, barCol, AlignLeft | AlignVCenter);

                    const int segW = 2, segStep = 3;
                    const int segCount = std::max(1, barW / segStep);
                    const int litCount = std::clamp(static_cast<int>(std::llround(ratio * segCount)), 0, segCount);
                    const Color track = Color::rgba(30, 41, 54);
                    for (int i = 0; i < segCount; ++i) {
                        fb_.fillRect({barX + i * segStep, barY, segW, barH}, i < litCount ? barCol : track);
                    }
                    fb_.drawText({rowRect.right() - rateW + 1, rowRect.y, rateW, rowRect.h}, *rf, rateText, theme.foreground, AlignRight | AlignVCenter);
                };

                drawSegRow({r.x, bodyY, r.w, rowH}, "↓", formatRate(rxBps), rxRatio, Color::rgba(0x2e, 0xd5, 0x73));
                drawSegRow({r.x, bodyY + rowH, r.w, r.bottom() - (bodyY + rowH) + 1}, "↑", formatRate(txBps), txRatio, Color::rgba(0x00, 0xa5, 0xdb));
                break;
            }

            case CardState::Kind::SensorDot: {
                if (r.w < 10 || r.h < 8) break;
                const std::string entity = p.getString("entity");
                const std::string source = resolveMetricKey(p, p.getString("source"));
                const std::string label = p.getString("label", entity);
                const std::string overflow = toLower(p.getString("overflow", "clip"));
                const std::string defaultState = toLower(p.getString("default_state", "unknown"));
                const Color onColor = p.contains("on_color") ? Color::fromHex(p.getString("on_color")) : Color::rgba(0x34, 0xc7, 0x59);
                const Color offColor = p.contains("off_color") ? Color::fromHex(p.getString("off_color")) : Color::rgba(0xff, 0x3b, 0x30);
                const Color unknownColor = p.contains("unknown_color") ? Color::fromHex(p.getString("unknown_color")) : Color::rgba(0xff, 0x9f, 0x0a);
                const int dotSize = std::clamp(p.getInt("dot_size", 5), 4, 12);
                const int fontSize = std::clamp(p.getInt("font_size", theme.fontSize), 6, 24);

                const std::string key = !source.empty() ? source : (!entity.empty() ? ("ha." + entity) : "");
                std::string curState = defaultState;
                if (!key.empty() && snapshot.contains(key)) {
                    curState = toLower(trim(snapshot.getString(key)));
                }

                Color ledColor = unknownColor;
                if (curState == "on" || curState == "true" || curState == "1") ledColor = onColor;
                else if (curState == "off" || curState == "false" || curState == "0") ledColor = offColor;

                const float cy = static_cast<float>(r.y) + r.h / 2.0f;
                const float cx = static_cast<float>(r.x) + dotSize / 2.0f;
                Color halo = ledColor;
                halo.a = 70;
                fb_.fillCircle(cx, cy, dotSize / 2.0f + 1.5f, halo);
                fb_.fillCircle(cx, cy, dotSize / 2.0f, ledColor);

                const int textX = r.x + dotSize + 2;
                const int textW = std::max(0, r.right() - textX + 1);
                if (textW <= 0) break;

                if (const BitmapFont *f = findBitmapFont(fontSize, false)) {
                    const Rect textRect{textX, r.y, textW, r.h};
                    fb_.setClip(textRect);
                    if (overflow == "scroll") {
                        const int fullW = fb_.measureText(*f, label);
                        if (fullW > textW) {
                            const int maxOff = fullW - textW;
                            st.scrollOffset += st.scrollDir * 4;
                            if (st.scrollOffset >= maxOff) { st.scrollOffset = maxOff; st.scrollDir = -1; }
                            else if (st.scrollOffset <= 0) { st.scrollOffset = 0; st.scrollDir = 1; }
                        } else {
                            st.scrollOffset = 0;
                        }
                        fb_.drawText(textRect, *f, label, theme.foreground, AlignLeft | AlignVCenter, -st.scrollOffset);
                    } else if (overflow == "clip") {
                        fb_.drawText(textRect, *f, label, theme.foreground, AlignLeft | AlignVCenter);
                    } else {
                        const std::string elided = fb_.elideText(*f, label, textW);
                        fb_.drawText(textRect, *f, elided, theme.foreground, AlignLeft | AlignVCenter);
                    }
                }
                break;
            }

            case CardState::Kind::HaSensor: {
                const std::string entity = p.getString("entity");
                const std::string prefix = p.getString("prefix");
                const std::string unit = p.getString("unit");
                const std::string iconName = p.getString("icon", "home");
                const int precision = p.getInt("precision", 1);
                const int iconSize = p.getInt("icon_size", 12);
                const int fontSize = p.getInt("font_size", theme.fontSize);
                const bool bold = p.getBool("bold", false);
                const int spacing = p.getInt("spacing", 3);
                const uint64_t minUpdateMs = static_cast<uint64_t>(std::max(0, p.getInt("min_update_ms", 10000)));

                if (!st.hasThrottle || minUpdateMs == 0 || (nowMs - st.lastThrottleMs) >= minUpdateMs) {
                    const std::string key = "ha." + entity;
                    std::string valText = "--";
                    bool hasEntityData = false;
                    if (!entity.empty() && snapshot.contains(key)) {
                        hasEntityData = true;
                        const std::string raw = trim(snapshot.getString(key));
                        if (!raw.empty() && raw != "unavailable" && raw != "unknown") {
                            bool ok = false;
                            const double num = snapshot.getDouble(key, 0.0, &ok);
                            const std::string u = unit.empty() ? snapshot.getString(key + ".unit_of_measurement") : unit;
                            if (ok) {
                                char buf[48];
                                std::snprintf(buf, sizeof(buf), "%.*f%s", precision, num, u.c_str());
                                valText = buf;
                            } else {
                                valText = raw + u;
                            }
                        }
                    }
                    st.cachedText = prefix.empty() ? valText : (prefix + " " + valText);
                    if (hasEntityData) {
                        st.lastThrottleMs = nowMs;
                        st.hasThrottle = true;
                    }
                }

                const IconAsset *ic = findIconAsset(iconName, iconSize);
                const BitmapFont *fnt = findBitmapFont(fontSize, bold);
                drawIconTextRow(fb_, r, ic, theme.accent, fnt, st.cachedText.empty() ? "--" : st.cachedText, theme.foreground, spacing);
                break;
            }

            case CardState::Kind::Placeholder: {
                const std::string label = p.getString("label", st.col.cardType);
                const std::string iconName = p.getString("icon");
                const int fontSize = p.getInt("font_size", theme.fontSize);
                const bool bold = p.getBool("bold", true);
                const int iconSize = p.getInt("icon_size", fontSize);
                const bool showIcon = p.getBool("show_icon", true);
                const int spacing = p.getInt("spacing", 3);

                const IconAsset *ic = (showIcon && !iconName.empty()) ? findIconAsset(iconName, iconSize) : nullptr;
                const BitmapFont *fnt = findBitmapFont(fontSize, bold);
                drawIconTextRow(fb_, r, ic, theme.accent, fnt, label, theme.foreground, spacing);
                break;
            }

            case CardState::Kind::Spacer:
                break;
        }
    }

    fb_.resetClip();
}

}  // namespace yad
