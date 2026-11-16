#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace yad {

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
    bool valid = true;

    static Color invalid() {
        Color c;
        c.valid = false;
        return c;
    }

    static Color rgba(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255) {
        return {r_, g_, b_, a_, true};
    }

    static Color fromHex(const std::string &hex, const Color &fallback = Color::invalid()) {
        if (hex.empty()) return fallback;
        size_t start = (hex[0] == '#') ? 1 : 0;
        const size_t len = hex.size() - start;
        if (len != 6 && len != 8) return fallback;
        char *end = nullptr;
        const uint32_t v = static_cast<uint32_t>(std::strtoul(hex.c_str() + start, &end, 16));
        if (!end || *end != '\0') return fallback;
        if (len == 6) {
            return {
                static_cast<uint8_t>((v >> 16) & 0xFF),
                static_cast<uint8_t>((v >> 8) & 0xFF),
                static_cast<uint8_t>(v & 0xFF),
                255,
                true
            };
        }
        return {
            static_cast<uint8_t>((v >> 24) & 0xFF),
            static_cast<uint8_t>((v >> 16) & 0xFF),
            static_cast<uint8_t>((v >> 8) & 0xFF),
            static_cast<uint8_t>(v & 0xFF),
            true
        };
    }

    uint16_t toRgb565() const {
        return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

    bool operator==(const Color &o) const {
        return valid == o.valid && r == o.r && g == o.g && b == o.b && a == o.a;
    }
    bool operator!=(const Color &o) const { return !(*this == o); }
};

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    int right() const { return x + w - 1; }
    int bottom() const { return y + h - 1; }
    bool empty() const { return w <= 0 || h <= 0; }
};

struct Margins {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    Margins operator+(const Margins &o) const {
        return {left + o.left, top + o.top, right + o.right, bottom + o.bottom};
    }
};

enum AlignFlags : uint8_t {
    AlignLeft    = 1 << 0,
    AlignRight   = 1 << 1,
    AlignHCenter = 1 << 2,
    AlignTop     = 1 << 3,
    AlignBottom  = 1 << 4,
    AlignVCenter = 1 << 5,
    AlignHMask   = AlignLeft | AlignRight | AlignHCenter,
    AlignVMask   = AlignTop | AlignBottom | AlignVCenter,
    AlignCenter  = AlignHCenter | AlignVCenter
};

struct SizeSpec {
    enum class Mode {
        Stretch,
        FixedPx,
        Percent
    };
    Mode mode = Mode::Stretch;
    double value = 1.0;
};

struct SnapshotValue {
    enum class Type { None, Double, Bool, String };
    Type type = Type::None;
    double dVal = 0.0;
    bool bVal = false;
    std::string sVal;

    static SnapshotValue fromDouble(double v) {
        SnapshotValue out;
        out.type = Type::Double;
        out.dVal = v;
        return out;
    }
    static SnapshotValue fromBool(bool v) {
        SnapshotValue out;
        out.type = Type::Bool;
        out.bVal = v;
        return out;
    }
    static SnapshotValue fromString(std::string v) {
        SnapshotValue out;
        out.type = Type::String;
        out.sVal = std::move(v);
        return out;
    }

    double asDouble(double fallback = 0.0, bool *ok = nullptr) const {
        if (type == Type::Double) {
            if (ok) *ok = true;
            return dVal;
        }
        if (type == Type::Bool) {
            if (ok) *ok = true;
            return bVal ? 1.0 : 0.0;
        }
        if (type == Type::String && !sVal.empty()) {
            char *end = nullptr;
            double v = std::strtod(sVal.c_str(), &end);
            if (end && end != sVal.c_str()) {
                while (*end == ' ' || *end == '\t') ++end;
                if (*end == '\0') {
                    if (ok) *ok = true;
                    return v;
                }
            }
        }
        if (ok) *ok = false;
        return fallback;
    }

    bool asBool(bool fallback = false) const {
        if (type == Type::Bool) return bVal;
        if (type == Type::Double) return dVal != 0.0;
        if (type == Type::String) {
            if (sVal == "true" || sVal == "on" || sVal == "1") return true;
            if (sVal == "false" || sVal == "off" || sVal == "0") return false;
        }
        return fallback;
    }

    std::string asString() const {
        if (type == Type::String) return sVal;
        if (type == Type::Bool) return bVal ? "true" : "false";
        if (type == Type::Double) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%.6g", dVal);
            return std::string(buf);
        }
        return {};
    }

    bool operator==(const SnapshotValue &o) const {
        if (type != o.type) return false;
        switch (type) {
            case Type::None: return true;
            case Type::Double: return dVal == o.dVal;
            case Type::Bool: return bVal == o.bVal;
            case Type::String: return sVal == o.sVal;
        }
        return false;
    }
    bool operator!=(const SnapshotValue &o) const { return !(*this == o); }
};

class Snapshot {
public:
    void set(const std::string &key, double v) { map_[key] = SnapshotValue::fromDouble(v); }
    void set(const std::string &key, bool v) { map_[key] = SnapshotValue::fromBool(v); }
    void set(const std::string &key, const std::string &v) { map_[key] = SnapshotValue::fromString(v); }
    void set(const std::string &key, const char *v) { map_[key] = SnapshotValue::fromString(v ? v : ""); }

    bool setIfChanged(const std::string &key, const SnapshotValue &val) {
        auto it = map_.find(key);
        if (it != map_.end() && it->second == val) return false;
        map_[key] = val;
        return true;
    }

    bool contains(const std::string &key) const { return map_.find(key) != map_.end(); }

    const SnapshotValue *find(const std::string &key) const {
        auto it = map_.find(key);
        return (it != map_.end()) ? &it->second : nullptr;
    }

    double getDouble(const std::string &key, double fallback = 0.0, bool *ok = nullptr) const {
        if (const SnapshotValue *v = find(key)) return v->asDouble(fallback, ok);
        if (ok) *ok = false;
        return fallback;
    }

    bool getBool(const std::string &key, bool fallback = false) const {
        if (const SnapshotValue *v = find(key)) return v->asBool(fallback);
        return fallback;
    }

    std::string getString(const std::string &key, const std::string &fallback = {}) const {
        if (const SnapshotValue *v = find(key)) return v->asString();
        return fallback;
    }

    void clear() { map_.clear(); }
    const std::unordered_map<std::string, SnapshotValue> &entries() const { return map_; }

private:
    std::unordered_map<std::string, SnapshotValue> map_;
};

}  // namespace yad
