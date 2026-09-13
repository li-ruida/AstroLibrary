#include "state.hpp"
#include <regex>
#include <unistd.h>
namespace astro {
static J readLegacy(const fs::path &dir, const std::string &name, J fallback) {
    auto path = dir / name;
    if (fs::exists(path))
        return readJSON(path, fallback);
    if (dir == home() / "Library/Application Support/AstroLibrary")
        for (auto app : {"AstroShelf", "SeeStarPic"}) {
            path = dir.parent_path() / app / name;
            if (fs::exists(path))
                return readJSON(path, fallback);
        }
    return fallback;
}
static J allTags(const J &catalog) {
    std::set<std::string> tags;
    for (auto &list : catalog.value("targetTags", J::object()))
        if (list.is_array())
            for (auto &tag : list)
                if (tag.is_string())
                    tags.insert(tag);
    return tags;
}
State::State(fs::path data, fs::path web, J overrides) : dataDir(normalized(data)), webRoot(normalized(web)) {
    defaultCache = overrides.contains("cacheDir") ? normalized(overrides["cacheDir"].get<std::string>())
                                                  : home() / "Library/Caches/AstroLibrary/fits";
    auto saved = readLegacy(dataDir, "settings.json", J::object());
    settings = {{"source", (home() / "Pictures/AstroLibrary").string()},
                {"libraryPath", (home() / "Pictures/AstroLibrary").string()},
                {"seestarSource", ""},
                {"destination", (home() / "Pictures/Seestar导出/MyWorks").string()},
                {"editedSource", ""},
                {"galleryMode", "auto"},
                {"exportScope", "all"},
                {"mergeTargets", true},
                {"fitsCacheDirectory", ""},
                {"port", 8765},
                {"auth", {{"enabled", false}, {"username", ""}, {"password", nullptr}}},
                {"r2", r2Config(J::object())}};
    settings.update(saved);
    std::string root =
        saved.value("libraryPath", saved.value("source", settings["source"].get<std::string>()));
    if (overrides.contains("source"))
        root = overrides["source"];
    settings["libraryPath"] = settings["source"] = normalized(root).string();
    if (!saved.contains("seestarSource") && root.find("com.zwoseestar.iscope") != root.npos)
        settings["seestarSource"] = root;
    for (auto key : {"destination", "port"})
        if (overrides.contains(key))
            settings[key] = overrides[key];
    for (auto key : {"destination", "editedSource", "seestarSource"})
        if (settings[key] != "")
            settings[key] = normalized(settings[key].get<std::string>()).string();
    require(settings["port"].is_number_integer() && settings["port"] >= 1024 && settings["port"] <= 65535,
            "端口必须介于 1024 和 65535");
    require(settings["mergeTargets"].is_boolean(), "目标合并设置必须为布尔值");
    require(settings["galleryMode"] == "auto" || settings["galleryMode"] == "source" ||
                settings["galleryMode"] == "edited",
            "图库模式无效");
    require(settings["exportScope"] == "all" || settings["exportScope"] == "single", "导出范围无效");
    settings["r2"] = r2Config(settings["r2"]);
    require(settings["auth"].is_object(), "账号保护配置错误");
    auto auth = settings["auth"];
    if (auth.value("enabled", false)) {
        require(auth.contains("password") && auth["password"].is_object() &&
                    auth["password"].value("algorithm", "") == "pbkdf2_sha256" &&
                    !auth.value("username", std::string()).empty(),
                "账号保护配置不完整");
        auto p = auth["password"];
        require(p.contains("iterations") && p["iterations"].is_number_integer() && p["iterations"] > 0 &&
                    p["iterations"] <= 10000000 && p.contains("salt") && p["salt"].is_string() &&
                    p.contains("hash") && p["hash"].is_string() &&
                    std::regex_match(p["salt"].get<std::string>(), std::regex("[0-9a-fA-F]{32}")) &&
                    std::regex_match(p["hash"].get<std::string>(), std::regex("[0-9a-fA-F]{64}")),
                "账号保护密码记录无效");
    }
    catalog = readLegacy(dataDir, "catalog.json", {{"targetTags", J::object()}});
    for (auto key : {"targetTags", "photoRatings", "targetWorkflow"}) {
        if (!catalog.contains(key))
            catalog[key] = J::object();
        require(catalog[key].is_object(), "图库元数据格式错误");
    }
    J tags = J::object();
    for (auto it = catalog["targetTags"].begin(); it != catalog["targetTags"].end(); ++it) {
        if (!it.value().is_array())
            continue;
        auto target = canonical(it.key());
        if (!tags.contains(target))
            tags[target] = J::array();
        for (auto &v : it.value())
            if (v.is_string() && std::find(tags[target].begin(), tags[target].end(), v) == tags[target].end())
                tags[target].push_back(v);
    }
    catalog["targetTags"] = tags;
    mapping = readLegacy(dataDir, "r2-mappings.json", {{"version", 1}, {"items", J::object()}});
    require(mapping.value("items", J()).is_object(), "R2 映射格式错误");
    for (const auto &record : mapping["items"])
        require(record.is_object(), "R2 映射记录格式错误");
    candidates = J::array();
    r2Status = {{"syncing", false}, {"pending", 0},     {"uploaded", 0},  {"skipped", 0},
                {"failed", 0},      {"lastSyncAt", ""}, {"lastError", ""}};
    importStatus = {{"running", false}, {"source", ""},  {"destination", ""}, {"imported", 0},
                    {"skipped", 0},     {"current", ""}, {"error", ""},       {"finished", false}};
    cacheJob = {{"running", false}, {"operation", ""}, {"phase", "idle"}, {"source", ""}, {"total", 0},
                {"processed", 0},   {"failed", 0},     {"current", ""},   {"error", ""},  {"nextSource", ""}};
    if (auto token = getenv("ASTROLIBRARY_APP_TOKEN"))
        appToken = token;
    fs::path cacheRoot = settings["fitsCacheDirectory"] == ""
                             ? defaultCache
                             : fs::path(settings["fitsCacheDirectory"].get<std::string>());
    if (cacheRoot == defaultCache)
        fs::create_directories(defaultCache.parent_path());
    cache = std::make_shared<FitsCache>(cacheRoot);
    writeJSON(dataDir / "settings.json", settings);
    writeJSON(dataDir / "catalog.json", catalog);
}
State::~State() {
    stopping = true;
    cancelCache = true;
    targetCleanup.cancelled = true;
    seestarCleanup.cancelled = true;
    jpegCleanup.cancelled = true;
    for (size_t i = 0;; i++) {
        std::thread t;
        {
            std::lock_guard l(threadsMutex);
            if (i >= workers.size())
                break;
            t = std::move(workers[i]);
        }
        if (t.joinable())
            t.join();
    }
}
void State::spawn(std::function<void()> work) {
    std::lock_guard l(threadsMutex);
    require(!stopping, "服务正在停止");
    workers.emplace_back([work = std::move(work)] {
        try {
            work();
        } catch (const std::exception &e) {
            fprintf(stderr, "Background error: %s\n", e.what());
        }
    });
}
J State::config() {
    std::lock_guard l(mutex);
    return settings;
}
void State::invalidate() {
    std::lock_guard l(libraryMutex);
    libraryAt = 0;
}
J State::r2StatusPayload() {
    std::lock_guard l(mutex);
    J result = r2Status;
    size_t n = 0;
    for (auto &r : mapping["items"])
        if (r.value("accountId", "") == settings["r2"]["accountId"] &&
            r.value("bucket", "") == settings["r2"]["bucket"])
            n++;
    result["mapped"] = n;
    return result;
}
J State::settingsPayload() {
    J s, c, meta;
    {
        std::lock_guard l(mutex);
        s = settings;
        c = candidates;
        meta = catalog;
    }
    J result = s;
    result.erase("auth");
    result.erase("fitsCacheDirectory");
    result["authEnabled"] = s["auth"].value("enabled", false);
    result["authUsername"] = result["authEnabled"] == true ? s["auth"].value("username", std::string()) : "";
    result["authConfigPath"] = (dataDir / "settings.json").string();
    result["tags"] = allTags(meta);
    result["sourceStatus"] = sourceStatus(s["source"].get<std::string>());
    result["editedStatus"] = sourceStatus(s["editedSource"].get<std::string>());
    result["candidates"] = c;
    result["r2"] = r2Public(s["r2"]);
    result["r2"].update(r2StatusPayload());
    result["r2"]["mappingPath"] = (dataDir / "r2-mappings.json").string();
    return result;
}
void State::updateSettings(const J &p) {
    bool changed = false;
    {
        std::lock_guard l(mutex);
        for (auto key : {"authEnabled", "authUsername", "authPassword", "auth", "fitsCacheDirectory"})
            require(!p.contains(key), "该设置请通过本机专用接口修改");
        J next = settings;
        for (auto key : {"libraryPath", "source", "seestarSource", "destination", "editedSource",
                         "galleryMode", "exportScope", "mergeTargets", "port", "r2"})
            if (p.contains(key))
                next[key] = p[key];
        if (p.contains("source") && !p.contains("libraryPath"))
            next["libraryPath"] = p["source"];
        for (auto key : {"libraryPath", "seestarSource", "destination", "editedSource"}) {
            require(next[key].is_string(), "目录路径必须为文本");
            auto path = trim(next[key]);
            require(path.find('\0') == path.npos, "目录路径无效");
            if (key == std::string("libraryPath") && path.empty())
                path = (home() / "Pictures/AstroLibrary").string();
            if (key == std::string("destination"))
                require(!path.empty(), "导出目录不能为空");
            next[key] = path.empty() ? "" : normalized(path).string();
        }
        next["source"] = next["libraryPath"];
        require(next["mergeTargets"].is_boolean(), "合并目标必须为布尔值");
        require(next["port"].is_number_integer() && next["port"] >= 1024 && next["port"] <= 65535,
                "端口必须介于 1024 和 65535");
        require(next["galleryMode"] == "auto" || next["galleryMode"] == "source" ||
                    next["galleryMode"] == "edited",
                "图库模式无效");
        require(next["exportScope"] == "all" || next["exportScope"] == "single", "导出范围无效");
        if (p.contains("r2"))
            next["r2"] = r2Config(p["r2"], settings["r2"]["secretAccessKey"]);
        bool context = next["source"] != settings["source"] ||
                       next["seestarSource"] != settings["seestarSource"] ||
                       next["destination"] != settings["destination"] ||
                       next["mergeTargets"] != settings["mergeTargets"];
        require(!context || !busy, "已有导入、导出或清理任务正在运行");
        changed = next["source"] != settings["source"];
        writeJSON(dataDir / "settings.json", next);
        if (next["r2"] != settings["r2"] || next["source"] != settings["source"] ||
            next["editedSource"] != settings["editedSource"]) {
            r2Generation++;
        }
        settings = next;
    }
    invalidate();
    if (changed)
        warmLibrary();
}
void State::setAuth(const J &p) {
    require(p.contains("enabled") && p["enabled"].is_boolean(), "账号开关必须为布尔值");
    require(!p.contains("username") || p["username"].is_string(), "用户名无效");
    require(!p.contains("password") || p["password"].is_string(), "密码无效");
    std::lock_guard l(mutex);
    J next = settings;
    auto auth = next["auth"];
    bool enabled = p["enabled"];
    std::string name = trim(p.value("username", auth.value("username", std::string()))),
                password = p.value("password", std::string());
    require(textLength(name) <= 64, "用户名过长");
    if (enabled)
        require(!name.empty(), "用户名不能为空");
    if (!password.empty()) {
        require(textLength(password) >= 8 && textLength(password) <= 1024, "密码必须为 8 到 1024 字符");
        auth["password"] = passwordHash(password);
    }
    if (enabled)
        require(auth.contains("password") && auth["password"].is_object(), "请先设置密码");
    auth["enabled"] = enabled;
    if (!name.empty())
        auth["username"] = name;
    next["auth"] = auth;
    writeJSON(dataDir / "settings.json", next);
    settings = next;
    sessions.clear();
}
bool State::nativeToken(const std::string &token) {
    return !appToken.empty() && constantEqual(appToken, token);
}
bool State::authenticated(const std::string &session) {
    std::lock_guard l(mutex);
    if (!settings["auth"].value("enabled", false))
        return true;
    auto it = sessions.find(session);
    if (it == sessions.end())
        return false;
    if (it->second < now()) {
        sessions.erase(it);
        return false;
    }
    return true;
}
std::string State::login(const J &p, const std::string &address) {
    require(p.value("password", J("")).is_string() && p.value("username", J("")).is_string(),
            "用户名或密码错误");
    std::lock_guard l(mutex);
    auto &attempt = loginAttempts[address];
    if (now() > attempt.second) {
        attempt.first = 0;
        attempt.second = now() + 60;
    }
    require(attempt.first < 5, "尝试次数过多，请在 60 秒后重试");
    auto a = settings["auth"];
    auto password = p.value("password", std::string());
    require(textLength(password) <= 1024, "密码过长");
    if (a.value("enabled", false)) {
        bool valid = verifyPassword(password, a.value("password", J()));
        valid =
            constantEqual(p.value("username", std::string()), a.value("username", std::string())) && valid;
        if (!valid) {
            attempt.first++;
            throw std::runtime_error("用户名或密码错误");
        }
        if (a["password"].value("iterations", 0) < 600000) {
            J next = settings;
            next["auth"]["password"] = passwordHash(password);
            writeJSON(dataDir / "settings.json", next);
            settings = next;
        }
    }
    attempt.first = 0;
    auto token = randomToken();
    for (auto it = sessions.begin(); it != sessions.end();)
        if (it->second < now())
            it = sessions.erase(it);
        else
            ++it;
    require(sessions.size() < 4096, "登录会话过多");
    sessions[token] = now() + 43200;
    return token;
}
J State::rating(const J &p) {
    require(p.contains("rating") && p["rating"].is_number_integer() && p["rating"] >= 0 && p["rating"] <= 5,
            "评分必须是 0 到 5 的整数");
    require(p.contains("id") && p["id"].is_string() && p.value("isEdited", J(false)).is_boolean(),
            "照片标识无效");
    std::lock_guard l(mutex);
    bool edited = p.value("isEdited", false);
    std::string root = settings[edited ? "editedSource" : "source"], id = p["id"];
    require(!root.empty() && p.value("scope", J()) == root, "图库目录已更改，请刷新后再评分");
    auto rel = edited && id.rfind("edited:", 0) == 0 ? id.substr(7) : id;
    auto path = safePath(root, rel);
    require(media(path) && fs::is_regular_file(path), "未找到当前图库中的照片");
    auto key = sha256("[" + J(root).dump(-1, ' ', true) + ", " +
                      J(path.lexically_relative(root).string()).dump(-1, ' ', true) + "]");
    J next = catalog;
    if (p["rating"] == 0)
        next["photoRatings"].erase(key);
    else
        next["photoRatings"][key] = p["rating"];
    writeJSON(dataDir / "catalog.json", next);
    catalog = next;
    return {{"ok", true}, {"id", id}, {"rating", p["rating"]}};
}
J State::workflow(const J &p) {
    require(p.value("target", J()).is_string(), "目标必须为文本");
    auto target = canonical(p["target"]);
    auto c = p.value("changes", J());
    require(!target.empty() && textLength(target) <= 240 && c.is_object() && !c.empty(), "工作流字段无效");
    for (auto it = c.begin(); it != c.end(); ++it) {
        if (it.key() == "favorite")
            require(it.value().is_boolean(), "收藏必须为布尔值");
        else if (it.key() == "stage")
            require(it.value() == "todo" || it.value() == "processing" || it.value() == "done",
                    "处理阶段无效");
        else if (it.key() == "notes")
            require(it.value().is_string() && textLength(it.value().get<std::string>()) <= 4000, "备注过长");
        else
            throw std::runtime_error("工作流字段无效");
    }
    std::lock_guard l(mutex);
    J next = catalog;
    J saved = {{"favorite", false}, {"stage", "todo"}, {"notes", ""}};
    saved.update(next["targetWorkflow"].value(target, J::object()));
    saved.update(c);
    next["targetWorkflow"][target] = saved;
    writeJSON(dataDir / "catalog.json", next);
    catalog = next;
    return {{"ok", true}, {"target", target}, {"workflow", saved}};
}
J State::tags(const J &p, bool remove) {
    require(p.value("target", J()).is_string(), "目标必须为文本");
    auto target = canonical(p["target"]);
    require(!target.empty(), "目标不能为空");
    J list = p.value("tags", J::array());
    {
        std::lock_guard l(mutex);
        if (remove) {
            require(p.value("tag", J()).is_string(), "标签无效");
            list = catalog["targetTags"].value(target, J::array());
            auto tag = trim(p["tag"]);
            list.erase(std::remove(list.begin(), list.end(), J(tag)), list.end());
        }
        require(list.is_array() && list.size() <= 100, "标签必须为数组，最多 100 个");
        std::set<std::string> tags;
        for (auto &v : list) {
            require(v.is_string(), "标签必须为文本");
            auto tag = trim(v);
            require(tag.size() <= 128, "标签过长");
            if (!tag.empty())
                tags.insert(tag);
        }
        list = tags;
        J next = catalog;
        next["targetTags"][target] = list;
        writeJSON(dataDir / "catalog.json", next);
        catalog = next;
    }
    invalidate();
    return {{"ok", true}, {"target", target}, {"tags", list}};
}
} // namespace astro
