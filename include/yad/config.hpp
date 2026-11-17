#pragma once

#include "yad/types.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace yad {

struct ParamValue {
    enum class Type { Null, Bool, Double, String };
    Type type = Type::Null;
    bool bVal = false;
    double dVal = 0.0;
    std::string sVal;

    std::string asString(const std::string &fallback = {}) const {
        if (type == Type::String) return sVal;
        if (type == Type::Bool) return bVal ? "true" : "false";
        if (type == Type::Double) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%.6g", dVal);
            return std::string(buf);
        }
        return fallback;
    }

    double asDouble(double fallback = 0.0) const {
        if (type == Type::Double) return dVal;
        if (type == Type::Bool) return bVal ? 1.0 : 0.0;
        if (type == Type::String && !sVal.empty()) {
            char *end = nullptr;
            double v = std::strtod(sVal.c_str(), &end);
            if (end && end != sVal.c_str()) return v;
        }
        return fallback;
    }

    int asInt(int fallback = 0) const {
        if (type == Type::Double) return static_cast<int>(dVal >= 0 ? dVal + 0.5 : dVal - 0.5);
        if (type == Type::Bool) return bVal ? 1 : 0;
        if (type == Type::String && !sVal.empty()) return std::atoi(sVal.c_str());
        return fallback;
    }

    bool asBool(bool fallback = false) const {
        if (type == Type::Bool) return bVal;
        if (type == Type::Double) return dVal != 0.0;
        if (type == Type::String) {
            if (sVal == "true" || sVal == "1" || sVal == "on") return true;
            if (sVal == "false" || sVal == "0" || sVal == "off") return false;
        }
        return fallback;
    }
};

class ParamMap {
public:
    bool contains(const std::string &key) const { return map_.find(key) != map_.end(); }
    void set(const std::string &key, ParamValue v) { map_[key] = std::move(v); }

    std::string getString(const std::string &key, const std::string &fallback = {}) const {
        auto it = map_.find(key);
        return (it != map_.end()) ? it->second.asString(fallback) : fallback;
    }
    double getDouble(const std::string &key, double fallback = 0.0) const {
        auto it = map_.find(key);
        return (it != map_.end()) ? it->second.asDouble(fallback) : fallback;
    }
    int getInt(const std::string &key, int fallback = 0) const {
        auto it = map_.find(key);
        return (it != map_.end()) ? it->second.asInt(fallback) : fallback;
    }
    bool getBool(const std::string &key, bool fallback = false) const {
        auto it = map_.find(key);
        return (it != map_.end()) ? it->second.asBool(fallback) : fallback;
    }

private:
    std::unordered_map<std::string, ParamValue> map_;
};

struct ThemeConfig {
    Color background = Color::rgba(0x0a, 0x0f, 0x16);
    Color foreground = Color::rgba(0xe6, 0xed, 0xf3);
    Color muted = Color::rgba(0x7d, 0x8f, 0xa3);
    Color accent = Color::rgba(0x00, 0xa5, 0xdb);
    std::string fontFamily = "Terminus";
    int fontSize = 12;
    int outerMargin = 4;
    int rowGap = 4;
    std::string defaultNinePatch = "card.9.png";
};

struct HomeAssistantConfig {
    std::string url;
    bool tlsVerify = true;
    std::string token;
    std::string tokenEnv = "HA_TOKEN";
    std::string tokenFile = "~/.config/G19Daemon/ha_token";
};

struct NodeExporterTarget {
    std::string name;  // e.g. "pc", "nas", or "default"
    std::vector<std::string> urls;
    int pollMs = 2000;
    int inactivePollMs = 0;  // 0 = pause polling when exporter isn't used on current page
    bool collectFilter = true;  // append ?collect[]=... when querying /metrics
    bool tlsVerify = true;
};

struct ColumnConfig {
    SizeSpec width{SizeSpec::Mode::Stretch, 1.0};
    SizeSpec cardWidth{SizeSpec::Mode::Stretch, 0.0};
    SizeSpec cardHeight{SizeSpec::Mode::Stretch, 0.0};
    uint8_t alignment = AlignCenter;
    bool hasAlignment = false;
    std::string ninePatch;
    Margins padding;
    Color background = Color::invalid();
    std::string cardType;
    ParamMap cardParams;
};

struct RowConfig {
    SizeSpec height{SizeSpec::Mode::Stretch, 1.0};
    int gap = 4;
    std::vector<ColumnConfig> columns;
};

struct PageConfig {
    std::string name = "Main";
    std::vector<RowConfig> rows;
};

struct DashboardConfig {
    std::string pluginId = "yad";
    std::string pluginName = "YAD";
    int refreshMs = 1000;
    ThemeConfig theme;
    HomeAssistantConfig homeAssistant;
    std::vector<NodeExporterTarget> nodeExporters;
    std::vector<PageConfig> pages;

    static bool loadFromJson(const std::string &jsonText, DashboardConfig &out, std::string *error = nullptr);
    static bool loadFromFile(const std::string &path, DashboardConfig &out, std::string *error = nullptr);
};

SizeSpec parseSizeSpecString(const std::string &s, const SizeSpec &fallback = {});
uint8_t parseAlignment(const std::string &text);
std::vector<std::string> extractWatchedEntities(const DashboardConfig &config);

}  // namespace yad
