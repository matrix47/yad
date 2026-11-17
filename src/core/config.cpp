#include "yad/config.hpp"

#if __has_include(<cjson/cJSON.h>)
#include <cjson/cJSON.h>
#else
#include <cJSON.h>
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace yad {

static std::string toLowerTrim(const std::string &in) {
    size_t a = 0;
    while (a < in.size() && std::isspace(static_cast<unsigned char>(in[a]))) ++a;
    size_t b = in.size();
    while (b > a && std::isspace(static_cast<unsigned char>(in[b - 1]))) --b;
    std::string out = in.substr(a, b - a);
    for (char &ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

SizeSpec parseSizeSpecString(const std::string &raw, const SizeSpec &fallback) {
    const std::string s = toLowerTrim(raw);
    if (s.empty() || s == "auto") return {SizeSpec::Mode::Stretch, 1.0};
    if (s.back() == '%') {
        char *end = nullptr;
        double v = std::strtod(s.substr(0, s.size() - 1).c_str(), &end);
        if (end && *end == '\0' && v > 0) return {SizeSpec::Mode::Percent, v};
    }
    if (s.back() == '*') {
        std::string num = toLowerTrim(s.substr(0, s.size() - 1));
        if (num.empty()) return {SizeSpec::Mode::Stretch, 1.0};
        char *end = nullptr;
        double v = std::strtod(num.c_str(), &end);
        if (end && *end == '\0' && v > 0) return {SizeSpec::Mode::Stretch, v};
    }
    std::string px = s;
    if (px.size() > 2 && px.substr(px.size() - 2) == "px") px.resize(px.size() - 2);
    char *end = nullptr;
    double v = std::strtod(px.c_str(), &end);
    if (end && *end == '\0' && v > 0) return {SizeSpec::Mode::FixedPx, v};
    return fallback;
}

static SizeSpec parseSizeSpecJson(const cJSON *item, const SizeSpec &fallback) {
    if (!item || cJSON_IsNull(item)) return fallback;
    if (cJSON_IsNumber(item)) return (item->valuedouble > 0) ? SizeSpec{SizeSpec::Mode::FixedPx, item->valuedouble} : fallback;
    if (cJSON_IsString(item) && item->valuestring) return parseSizeSpecString(item->valuestring, fallback);
    return fallback;
}

uint8_t parseAlignment(const std::string &text) {
    uint8_t align = 0;
    std::string cur;
    auto flush = [&]() {
        const std::string tok = toLowerTrim(cur);
        cur.clear();
        if (tok == "left") align |= AlignLeft;
        else if (tok == "right") align |= AlignRight;
        else if (tok == "hcenter") align |= AlignHCenter;
        else if (tok == "top") align |= AlignTop;
        else if (tok == "bottom") align |= AlignBottom;
        else if (tok == "vcenter") align |= AlignVCenter;
        else if (tok == "center") align |= AlignCenter;
    };
    for (char ch : text) {
        if (ch == '|' || ch == ',' || std::isspace(static_cast<unsigned char>(ch))) flush();
        else cur.push_back(ch);
    }
    flush();
    if (align == 0) return AlignCenter;
    if ((align & AlignHMask) == 0) align |= AlignHCenter;
    if ((align & AlignVMask) == 0) align |= AlignVCenter;
    return align;
}

static Margins parseMarginsJson(const cJSON *val) {
    if (!val) return {};
    if (cJSON_IsNumber(val)) {
        const int m = static_cast<int>(val->valuedouble);
        return {m, m, m, m};
    }
    if (cJSON_IsArray(val)) {
        const int n = cJSON_GetArraySize(val);
        if (n == 4) {
            return {
                static_cast<int>(cJSON_GetArrayItem(val, 3)->valuedouble),
                static_cast<int>(cJSON_GetArrayItem(val, 0)->valuedouble),
                static_cast<int>(cJSON_GetArrayItem(val, 1)->valuedouble),
                static_cast<int>(cJSON_GetArrayItem(val, 2)->valuedouble)
            };
        }
        if (n == 2) {
            const int v = static_cast<int>(cJSON_GetArrayItem(val, 0)->valuedouble);
            const int h = static_cast<int>(cJSON_GetArrayItem(val, 1)->valuedouble);
            return {h, v, h, v};
        }
    }
    return {};
}

static std::string jsonStr(const cJSON *obj, const char *key, const std::string &fallback = {}) {
    const cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    return (v && cJSON_IsString(v) && v->valuestring) ? std::string(v->valuestring) : fallback;
}

static int jsonInt(const cJSON *obj, const char *key, int fallback = 0) {
    const cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    return (v && cJSON_IsNumber(v)) ? static_cast<int>(v->valuedouble) : fallback;
}

static bool jsonBool(const cJSON *obj, const char *key, bool fallback = false) {
    const cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    if (!v) return fallback;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return fallback;
}

static ParamMap parseParamMap(const cJSON *obj) {
    ParamMap pm;
    if (!obj || !cJSON_IsObject(obj)) return pm;
    for (const cJSON *child = obj->child; child != nullptr; child = child->next) {
        if (!child->string) continue;
        ParamValue pv;
        if (cJSON_IsBool(child)) {
            pv.type = ParamValue::Type::Bool;
            pv.bVal = static_cast<bool>(cJSON_IsTrue(child));
        } else if (cJSON_IsNumber(child)) {
            pv.type = ParamValue::Type::Double;
            pv.dVal = child->valuedouble;
        } else if (cJSON_IsString(child) && child->valuestring) {
            pv.type = ParamValue::Type::String;
            pv.sVal = child->valuestring;
        }
        pm.set(child->string, std::move(pv));
    }
    return pm;
}

static std::vector<RowConfig> parseRows(const cJSON *rowsArr) {
    std::vector<RowConfig> rows;
    if (!rowsArr || !cJSON_IsArray(rowsArr)) return rows;
    const int rowCount = cJSON_GetArraySize(rowsArr);
    rows.reserve(rowCount);
    for (int i = 0; i < rowCount; ++i) {
        const cJSON *ro = cJSON_GetArrayItem(rowsArr, i);
        if (!cJSON_IsObject(ro)) continue;
        RowConfig row;
        row.height = parseSizeSpecJson(cJSON_GetObjectItemCaseSensitive(ro, "height"), {SizeSpec::Mode::Stretch, 1.0});
        row.gap = jsonInt(ro, "gap", 4);

        const cJSON *colsArr = cJSON_GetObjectItemCaseSensitive(ro, "columns");
        if (colsArr && cJSON_IsArray(colsArr)) {
            const int colCount = cJSON_GetArraySize(colsArr);
            row.columns.reserve(colCount);
            for (int j = 0; j < colCount; ++j) {
                const cJSON *co = cJSON_GetArrayItem(colsArr, j);
                if (!cJSON_IsObject(co)) continue;
                ColumnConfig col;
                col.width = parseSizeSpecJson(cJSON_GetObjectItemCaseSensitive(co, "width"), {SizeSpec::Mode::Stretch, 1.0});
                col.cardWidth = parseSizeSpecJson(cJSON_GetObjectItemCaseSensitive(co, "card_width"), {SizeSpec::Mode::Stretch, 0.0});
                col.cardHeight = parseSizeSpecJson(cJSON_GetObjectItemCaseSensitive(co, "card_height"), {SizeSpec::Mode::Stretch, 0.0});
                const cJSON *al = cJSON_GetObjectItemCaseSensitive(co, "alignment");
                if (al && cJSON_IsString(al) && al->valuestring) {
                    col.hasAlignment = true;
                    col.alignment = parseAlignment(al->valuestring);
                }
                col.ninePatch = jsonStr(co, "ninepatch", "");
                col.padding = parseMarginsJson(cJSON_GetObjectItemCaseSensitive(co, "padding"));
                const std::string bgHex = jsonStr(co, "background", "");
                if (!bgHex.empty()) col.background = Color::fromHex(bgHex);
                col.cardType = jsonStr(co, "card_type", "spacer");
                col.cardParams = parseParamMap(cJSON_GetObjectItemCaseSensitive(co, "card_params"));
                row.columns.push_back(std::move(col));
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

static void parseNodeExporters(const cJSON *ne, std::vector<NodeExporterTarget> &out) {
    if (!ne) return;
    auto pushUrlArray = [](const cJSON *arr, std::vector<std::string> &dst) {
        const int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; ++i) {
            const cJSON *it = cJSON_GetArrayItem(arr, i);
            if (cJSON_IsString(it) && it->valuestring && it->valuestring[0] != '\0') {
                dst.emplace_back(it->valuestring);
            }
        }
    };

    if (cJSON_IsArray(ne)) {
        NodeExporterTarget t;
        t.name = "default";
        pushUrlArray(ne, t.urls);
        if (!t.urls.empty()) out.push_back(std::move(t));
        return;
    }

    if (!cJSON_IsObject(ne)) return;

    auto parseTargetOpts = [](const cJSON *obj, NodeExporterTarget &t) {
        t.pollMs = std::max(250, jsonInt(obj, "poll_ms", 2000));
        const int inact = jsonInt(obj, "inactive_poll_ms", 0);
        t.inactivePollMs = (inact <= 0) ? 0 : std::max(250, inact);
        t.collectFilter = jsonBool(obj, "collect_filter", true);
        t.tlsVerify = jsonBool(obj, "tls_verify", true);
    };

    // Legacy object form: { "poll_ms": 2000, "urls": [...] }
    const cJSON *legacyUrls = cJSON_GetObjectItemCaseSensitive(ne, "urls");
    if (cJSON_IsArray(legacyUrls)) {
        NodeExporterTarget t;
        t.name = "default";
        parseTargetOpts(ne, t);
        pushUrlArray(legacyUrls, t.urls);
        if (!t.urls.empty()) out.push_back(std::move(t));
        return;
    }

    // Named map form: { "pc": "http://...", "nas": { "urls": [...], "poll_ms": 2000 } }
    for (const cJSON *child = ne->child; child != nullptr; child = child->next) {
        if (!child->string) continue;
        NodeExporterTarget t;
        t.name = child->string;
        if (cJSON_IsString(child) && child->valuestring && child->valuestring[0] != '\0') {
            t.urls.emplace_back(child->valuestring);
        } else if (cJSON_IsArray(child)) {
            pushUrlArray(child, t.urls);
        } else if (cJSON_IsObject(child)) {
            parseTargetOpts(child, t);
            const std::string singleUrl = jsonStr(child, "url");
            if (!singleUrl.empty()) t.urls.push_back(singleUrl);
            const cJSON *arr = cJSON_GetObjectItemCaseSensitive(child, "urls");
            if (cJSON_IsArray(arr)) pushUrlArray(arr, t.urls);
        }
        if (!t.urls.empty()) out.push_back(std::move(t));
    }
}

bool DashboardConfig::loadFromJson(const std::string &jsonText, DashboardConfig &out, std::string *error) {
    cJSON *root = cJSON_Parse(jsonText.c_str());
    if (!root || !cJSON_IsObject(root)) {
        if (error) {
            const char *errPtr = cJSON_GetErrorPtr();
            *error = errPtr ? std::string("JSON parse error near: ") + std::string(errPtr).substr(0, 32)
                            : std::string("Invalid JSON root object");
        }
        cJSON_Delete(root);
        return false;
    }

    DashboardConfig cfg;
    const cJSON *po = cJSON_GetObjectItemCaseSensitive(root, "plugin");
    if (cJSON_IsObject(po)) {
        cfg.pluginId = jsonStr(po, "id", cfg.pluginId);
        cfg.pluginName = jsonStr(po, "name", cfg.pluginName);
    }

    cfg.refreshMs = std::max(100, jsonInt(root, "refresh_ms", 1000));

    const cJSON *to = cJSON_GetObjectItemCaseSensitive(root, "theme");
    if (cJSON_IsObject(to)) {
        if (auto s = jsonStr(to, "background"); !s.empty()) cfg.theme.background = Color::fromHex(s, cfg.theme.background);
        if (auto s = jsonStr(to, "foreground"); !s.empty()) cfg.theme.foreground = Color::fromHex(s, cfg.theme.foreground);
        if (auto s = jsonStr(to, "muted"); !s.empty()) cfg.theme.muted = Color::fromHex(s, cfg.theme.muted);
        if (auto s = jsonStr(to, "accent"); !s.empty()) cfg.theme.accent = Color::fromHex(s, cfg.theme.accent);
        cfg.theme.fontFamily = jsonStr(to, "font_family", cfg.theme.fontFamily);
        cfg.theme.fontSize = jsonInt(to, "font_size", cfg.theme.fontSize);
        cfg.theme.outerMargin = jsonInt(to, "outer_margin", cfg.theme.outerMargin);
        cfg.theme.rowGap = jsonInt(to, "row_gap", cfg.theme.rowGap);
        cfg.theme.defaultNinePatch = jsonStr(to, "default_ninepatch", cfg.theme.defaultNinePatch);
    }

    const cJSON *ha = cJSON_GetObjectItemCaseSensitive(root, "home_assistant");
    if (cJSON_IsObject(ha)) {
        cfg.homeAssistant.url = jsonStr(ha, "url");
        cfg.homeAssistant.tlsVerify = jsonBool(ha, "tls_verify", true);
        cfg.homeAssistant.token = jsonStr(ha, "token");
        cfg.homeAssistant.tokenEnv = jsonStr(ha, "token_env", cfg.homeAssistant.tokenEnv);
        cfg.homeAssistant.tokenFile = jsonStr(ha, "token_file", cfg.homeAssistant.tokenFile);
    }

    parseNodeExporters(cJSON_GetObjectItemCaseSensitive(root, "node_exporters"), cfg.nodeExporters);

    const cJSON *pagesArr = cJSON_GetObjectItemCaseSensitive(root, "pages");
    if (cJSON_IsArray(pagesArr)) {
        const int n = cJSON_GetArraySize(pagesArr);
        for (int i = 0; i < n; ++i) {
            const cJSON *pObj = cJSON_GetArrayItem(pagesArr, i);
            if (!cJSON_IsObject(pObj)) continue;
            PageConfig page;
            page.name = jsonStr(pObj, "name", "Page");
            const int pr = jsonInt(pObj, "refresh_ms", 0);
            page.refreshMs = (pr > 0) ? std::max(100, pr) : 0;
            page.rows = parseRows(cJSON_GetObjectItemCaseSensitive(pObj, "rows"));
            cfg.pages.push_back(std::move(page));
        }
    } else {
        const cJSON *rowsArr = cJSON_GetObjectItemCaseSensitive(root, "rows");
        if (cJSON_IsArray(rowsArr)) {
            PageConfig page;
            page.name = "Main";
            page.rows = parseRows(rowsArr);
            cfg.pages.push_back(std::move(page));
        }
    }

    cJSON_Delete(root);
    out = std::move(cfg);
    return true;
}

bool DashboardConfig::loadFromFile(const std::string &path, DashboardConfig &out, std::string *error) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        if (error) *error = "Failed to open config: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return loadFromJson(ss.str(), out, error);
}

std::vector<std::string> extractWatchedEntities(const DashboardConfig &config) {
    std::unordered_set<std::string> set;
    for (const auto &page : config.pages) {
        for (const auto &row : page.rows) {
            for (const auto &col : row.columns) {
                auto addIf = [&](const std::string &s) {
                    if (!s.empty()) set.insert(s);
                };
                addIf(col.cardParams.getString("entity"));
                addIf(col.cardParams.getString("condition_entity"));
            }
        }
    }
    return std::vector<std::string>(set.begin(), set.end());
}

}  // namespace yad
