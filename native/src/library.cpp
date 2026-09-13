#include "state.hpp"
#include <ctime>
#include <regex>
namespace astro {
static std::string normStem(std::string s) {
    s = lower(s);
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    std::replace(s.begin(), s.end(), '-', '_');
    for (std::string suffix : {"_edited", "_edit", "_final", "_修图", "_后期", "_result"})
        if (s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix)
            s.resize(s.size() - suffix.size());
    return s;
}
static J metadata(const fs::path &p, const J &h, const J &st) {
    double timestamp = st[3].get<int64_t>() / 1e9;
    bool parsed = false;
    auto date = h.value("DATE-OBS", h.value("DATE-EXP", std::string()));
    if (!date.empty()) {
        tm t{};
        const char *end = strptime(date.c_str(), "%Y-%m-%dT%H:%M:%S", &t);
        if (!end)
            end = strptime(date.c_str(), "%Y-%m-%d %H:%M:%S", &t);
        if (end) {
            if (*end == '.') {
                end++;
                while (isdigit(*end))
                    end++;
            }
            int offset = 0;
            if (*end == '+' || *end == '-') {
                int sign = *end++ == '+' ? 1 : -1;
                int hh = 0, mm = 0;
                if (sscanf(end, "%2d:%2d", &hh, &mm) == 2 || sscanf(end, "%2d%2d", &hh, &mm) >= 1)
                    offset = sign * (hh * 3600 + mm * 60);
            }
            timestamp = timegm(&t) - offset;
            parsed = true;
        }
    }
    if (!parsed) {
        static std::regex r("_(\\d{8})-(\\d{6})");
        std::smatch m;
        std::string stem = p.stem();
        if (std::regex_search(stem, m, r)) {
            tm t{};
            std::string text = m[1].str() + m[2].str();
            if (strptime(text.c_str(), "%Y%m%d%H%M%S", &t)) {
                t.tm_isdst = -1;
                timestamp = mktime(&t);
            }
        }
    }
    auto captured = isoTime(timestamp);
    auto device = h.value("CREATOR", h.value("TELESCOP", std::string("未知设备")));
    static const std::regex devicePrefix("^ZWO\\s+");
    device = trim(std::regex_replace(device, devicePrefix, ""));
    if (device.empty())
        device = "未知设备";
    return {{"capturedAt", captured},
            {"captureDate", captured.substr(0, 10)},
            {"device", device},
            {"filter", trim(h.value("FILTER", std::string()))},
            {"exposure", trim(h.value("EXPTIME", h.value("EXPOSURE", std::string())))}};
}
static void sortItems(J &items) {
    std::stable_sort(items.begin(), items.end(),
                     [](const J &a, const J &b) { return a.at("capturedAt") > b.at("capturedAt"); });
}
J State::scanLibrary(const J &s, const std::shared_ptr<FitsCache> &fits) {
    fs::path root = s.at("source").get<std::string>();
    bool merge = s.at("mergeTargets");
    J status = sourceStatus(root), editedStatus = sourceStatus(s.at("editedSource").get<std::string>());
    J result = {
        {"source", root.string()},
        {"items", J::array()},
        {"editedItems", J::array()},
        {"editedByTarget", J::object()},
        {"subCollections", J::object()},
        {"categories", J::array()},
        {"tags", J::array()},
        {"facets",
         {{"targets", J::array()}, {"devices", J::array()}, {"dates", J::array()}, {"tags", J::array()}}},
        {"stats",
         {{"total", 0},
          {"photos", 0},
          {"raw", 0},
          {"subPhotos", 0},
          {"subRaw", 0},
          {"bytes", 0},
          {"editedPhotos", 0},
          {"editedBytes", 0}}},
        {"sourceStatus", status},
        {"editedStatus", editedStatus}};
    std::vector<fs::path> paths;
    if (status["accessible"] == true) {
        try {
            paths = mediaFiles(root);
        } catch (const std::exception &e) {
            result["sourceStatus"]["accessible"] = false;
            result["sourceStatus"]["reason"] = e.what();
        }
    }
    result["sourceStatus"]["mediaCount"] = paths.size();
    std::map<fs::path, J> headers;
    for (auto &p : paths)
        if (raw(p)) {
            try {
                headers[p] = fits->header(p);
            } catch (...) {
            }
        }
    std::map<std::string, int> counts;
    std::map<std::string, std::map<std::string, J>> groups;
    for (auto &p : paths) {
        try {
            auto st = regularStamp(root, p.lexically_relative(root));
            auto rel = p.lexically_relative(root);
            auto [target, sub] = mediaTarget(rel, merge);
            fs::path hp = p;
            if (!raw(p)) {
                hp.replace_extension(".fit");
                if (!headers.count(hp))
                    hp.replace_extension(".fits");
            }
            auto h = headers.count(hp) ? headers[hp] : J::object();
            auto id = rel.string();
            std::string kind = raw(p) ? "raw" : "photo";
            J item = {{"id", id},
                      {"name", p.stem().string()},
                      {"category", target},
                      {"kind", kind},
                      {"extension", lower(p.extension()).substr(1)},
                      {"size", st[2]},
                      {"modified", isoTime(st[3].get<int64_t>() / 1e9, false)},
                      {"url", std::string(raw(p) ? "/api/fits-preview/" : "/media/") + urlEncode(id) +
                                  "?v=" + std::to_string(st[3].get<int64_t>())},
                      {"originalUrl", "/media/" + urlEncode(id)},
                      {"isSub", sub}};
            item.update(metadata(p, h, st));
            if (sub) {
                std::string date = item["captureDate"], device = item["device"],
                            groupKey = date + "|" + device;
                auto &g = groups[target][groupKey];
                if (g.is_null())
                    g = {{"id", target + "|" + groupKey},
                         {"date", date},
                         {"device", device},
                         {"folder", rel.parent_path().string()},
                         {"photos", 0},
                         {"raw", 0},
                         {"filters", J::array()},
                         {"items", J::array()}};
                g[kind == "raw" ? "raw" : "photos"] = g[kind == "raw" ? "raw" : "photos"].get<int>() + 1;
                if (item["filter"] != "" &&
                    std::find(g["filters"].begin(), g["filters"].end(), item["filter"]) == g["filters"].end())
                    g["filters"].push_back(item["filter"]);
                g["items"].push_back(item);
                auto key = kind == "raw" ? "subRaw" : "subPhotos";
                result["stats"][key] = result["stats"][key].get<int>() + 1;
            } else {
                result["items"].push_back(item);
                counts[target]++;
                auto key = kind == "raw" ? "raw" : "photos";
                result["stats"][key] = result["stats"][key].get<int>() + 1;
                result["stats"]["bytes"] = result["stats"]["bytes"].get<int64_t>() + st[2].get<int64_t>();
            }
            result["stats"]["total"] = result["stats"]["total"].get<int>() + 1;
        } catch (const std::exception &e) {
            fprintf(stderr, "Scan skipped: %s\n", e.what());
        }
    }
    sortItems(result["items"]);
    for (auto &[name, count] : counts)
        result["categories"].push_back({{"name", name}, {"count", count}});
    for (auto &[target, buckets] : groups) {
        J c = {{"target", target}, {"photos", 0}, {"raw", 0}, {"groups", J::array()}};
        for (auto it = buckets.rbegin(); it != buckets.rend(); ++it) {
            auto &g = it->second;
            sortItems(g["items"]);
            std::sort(g["filters"].begin(), g["filters"].end());
            c["photos"] = c["photos"].get<int>() + g["photos"].get<int>();
            c["raw"] = c["raw"].get<int>() + g["raw"].get<int>();
            c["groups"].push_back(g);
        }
        result["subCollections"][target] = c;
    }
    if (editedStatus["accessible"] == true) {
        fs::path edited = s["editedSource"].get<std::string>();
        std::map<std::string, J> masters;
        std::set<std::string> targets;
        for (auto &item : result["items"]) {
            targets.insert(item["category"]);
            masters[normStem(item["name"])] = item;
        }
        try {
            for (auto &p : mediaFiles(edited)) {
                if (!photo(p))
                    continue;
                try {
                    auto rel = p.lexically_relative(edited);
                    std::string target = "未归类修图";
                    J master;
                    for (const auto &part : rel.parent_path()) {
                        auto name = merge ? canonical(part.string()) : part.string();
                        if (targets.count(name)) {
                            target = name;
                            break;
                        }
                    }
                    auto stem = normStem(p.stem());
                    if (target == "未归类修图") {
                        auto it = masters.find(stem);
                        if (it != masters.end()) {
                            master = it->second;
                            target = master["category"];
                        } else {
                            size_t best = 0;
                            for (auto &t : targets) {
                                auto n = lower(t);
                                n.erase(std::remove(n.begin(), n.end(), ' '), n.end());
                                if (n.size() > best && stem.find(n) != stem.npos) {
                                    best = n.size();
                                    target = t;
                                }
                            }
                        }
                    }
                    if (target == "未归类修图") {
                        auto name = rel.has_parent_path() ? rel.begin()->string() : p.stem().string();
                        target = merge ? canonical(name) : name;
                    }
                    auto st = regularStamp(edited, rel);
                    J item = {{"id", "edited:" + rel.string()},
                              {"name", p.stem().string()},
                              {"category", target},
                              {"kind", "photo"},
                              {"extension", lower(p.extension()).substr(1)},
                              {"size", st[2]},
                              {"modified", isoTime(st[3].get<int64_t>() / 1e9, false)},
                              {"url", "/edited-media/" + urlEncode(rel.string())},
                              {"isSub", false},
                              {"isEdited", true},
                              {"masterId", master.is_object() ? master["id"] : J()},
                              {"versionLabel", "精选成片"}};
                    item.update(metadata(p, J::object(), st));
                    if (master.is_object())
                        for (auto key : {"capturedAt", "captureDate", "device", "filter", "exposure"})
                            item[key] = master[key];
                    result["editedItems"].push_back(item);
                    result["stats"]["editedBytes"] =
                        result["stats"]["editedBytes"].get<int64_t>() + st[2].get<int64_t>();
                } catch (...) {
                }
            }
            sortItems(result["editedItems"]);
            result["stats"]["editedPhotos"] = result["editedItems"].size();
            result["editedStatus"]["mediaCount"] = result["editedItems"].size();
        } catch (const std::exception &e) {
            result["editedStatus"]["reason"] = e.what();
        }
    }
    return result;
}
void State::decorate(J &p, const J &s, const J &meta) {
    J records;
    {
        std::lock_guard l(mutex);
        records = mapping["items"];
    }
    auto decorateItem = [&](J &item) {
        bool edited = item.value("isEdited", false);
        std::string scope = s[edited ? "editedSource" : "source"], id = item["id"];
        if (edited && id.rfind("edited:", 0) == 0)
            id = id.substr(7);
        auto key = sha256("[" + J(scope).dump(-1, ' ', true) + ", " + J(id).dump(-1, ' ', true) + "]");
        auto rating = meta["photoRatings"].value(key, J(0));
        if (!rating.is_number_integer() || rating < 0 || rating > 5)
            rating = 0;
        item["rating"] = rating;
        item["ratingScope"] = scope;
        item["tags"] = meta["targetTags"].value(canonical(item["category"]), J::array());
        auto record = records.value(item["id"].get<std::string>(), J::object());
        if (!record.empty()) {
            item["remoteUri"] = record.value("remoteUri", "");
            item["publicUri"] = record.value("publicUri", "");
            item["remoteUploadedAt"] = record.value("uploadedAt", "");
        }
    };
    for (auto &i : p["items"])
        decorateItem(i);
    for (auto &c : p["subCollections"])
        for (auto &g : c["groups"])
            if (g.contains("items"))
                for (auto &i : g["items"])
                    decorateItem(i);
    p["editedByTarget"] = J::object();
    for (auto &i : p["editedItems"]) {
        decorateItem(i);
        auto target = i["category"].get<std::string>();
        if (!p["editedByTarget"].contains(target))
            p["editedByTarget"][target] = J::array();
        p["editedByTarget"][target].push_back(i);
    }
    std::set<std::string> tags;
    for (auto &list : meta["targetTags"])
        for (auto &t : list)
            if (t.is_string())
                tags.insert(t);
    p["tags"] = tags;
    std::map<std::string, std::map<std::string, int>> counts;
    for (auto &i : p["items"]) {
        for (auto [facet, field] :
             {std::pair{"targets", "category"}, {"devices", "device"}, {"dates", "captureDate"}}) {
            auto v = i.value(field, std::string());
            counts[facet][v.empty() ? "未知" : v]++;
        }
        for (auto &tag : i["tags"])
            counts["tags"][tag.get<std::string>()]++;
    }
    for (auto facet : {"targets", "devices", "dates", "tags"}) {
        p["facets"][facet] = J::array();
        for (auto &[value, count] : counts[facet])
            p["facets"][facet].push_back({{"value", value}, {"count", count}});
    }
    p["targetWorkflow"] = meta["targetWorkflow"];
}
// Project directly from the scan result: copying all SUB frames and then
// erasing them makes even a cache hit scale with the size of the whole library.
static J summarizeLibrary(const J &source) {
    J summary = J::object();
    for (auto it = source.begin(); it != source.end(); ++it)
        if (it.key() != "subCollections")
            summary[it.key()] = it.value();
    summary["subCollections"] = J::object();
    for (auto it = source.at("subCollections").begin(); it != source.at("subCollections").end(); ++it) {
        J collection = J::object();
        for (auto field = it.value().begin(); field != it.value().end(); ++field)
            if (field.key() != "groups")
                collection[field.key()] = field.value();
        collection["groups"] = J::array();
        for (const auto &group : it.value().at("groups")) {
            J brief = J::object();
            for (auto field = group.begin(); field != group.end(); ++field)
                if (field.key() != "items")
                    brief[field.key()] = field.value();
            collection["groups"].push_back(std::move(brief));
        }
        summary["subCollections"][it.key()] = std::move(collection);
    }
    return summary;
}
J State::libraryView(bool refresh, bool full, const std::string &subTarget) {
    J s, meta;
    std::shared_ptr<FitsCache> fits;
    {
        std::lock_guard l(mutex);
        s = settings;
        meta = catalog;
        fits = cache;
    }
    auto key = J::array({s["source"], s["editedSource"], s["mergeTargets"]}).dump();
    J p, scanInfo;
    bool cached;
    {
        std::lock_guard l(libraryMutex);
        cached = !refresh && !libraryCache.is_null() && libraryKey == key && now() - libraryAt < 20;
        if (!cached) {
            double start = now();
            libraryCache = scanLibrary(s, fits);
            librarySummary = summarizeLibrary(libraryCache);
            libraryKey = key;
            libraryAt = now();
            scan = {{"lastScanAt", isoTime(now())}, {"durationMs", int((now() - start) * 1000)}};
        }
        if (subTarget.empty())
            p = full ? libraryCache : librarySummary;
        else {
            require(libraryCache["subCollections"].contains(subTarget), "未找到对应的 SUB 集合");
            p = {{"items", J::array()},
                 {"editedItems", J::array()},
                 {"subCollections", {{subTarget, libraryCache["subCollections"][subTarget]}}}};
        }
        scanInfo = scan;
    }
    decorate(p, s, meta);
    if (!cached && s["r2"]["enabled"] == true && s["r2"]["autoUpload"] == true)
        try {
            syncR2();
        } catch (...) {
        }
    if (!subTarget.empty())
        return {{"ok", true}, {"collection", std::move(p["subCollections"][subTarget])}};
    p["scan"] = scanInfo;
    p["scan"]["cached"] = cached;
    p["settings"] = settingsPayload();
    return p;
}
J State::library(bool refresh, bool full) {
    return libraryView(refresh, full, "");
}
J State::subCollection(const std::string &target) {
    require(!target.empty(), "目标不能为空");
    return libraryView(false, false, target);
}
} // namespace astro
