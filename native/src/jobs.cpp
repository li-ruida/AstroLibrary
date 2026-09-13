#include "state.hpp"
#include <regex>
namespace astro {
Cleanup::Cleanup() {
    status = {{"running", false}, {"phase", "idle"},   {"jobId", ""},         {"token", ""},  {"target", ""},
              {"source", ""},     {"destination", ""}, {"checked", 0},        {"count", 0},   {"masters", 0},
              {"subs", 0},        {"bytes", 0},        {"moved", 0},          {"skipped", 0}, {"failed", 0},
              {"current", ""},    {"error", ""},       {"errors", J::array()}};
}
J Cleanup::snapshot() {
    std::lock_guard l(mutex);
    if (!plan.is_null() && now() - plannedAt >= 600) {
        plan = nullptr;
        status["phase"] = "expired";
        status["token"] = "";
    }
    return status;
}
void Cleanup::update(const J &v) {
    std::lock_guard l(mutex);
    status.update(v);
}
J State::cacheStatus() {
    J job;
    {
        std::lock_guard l(cacheJobMutex);
        job = cacheJob;
        job["nextSource"] = pendingCache;
    }
    std::shared_ptr<FitsCache> c;
    std::string configured;
    {
        std::lock_guard l(mutex);
        c = cache;
        configured = settings["fitsCacheDirectory"];
    }
    return {{"ok", true},
            {"cache", c->info()},
            {"job", job},
            {"configuredDirectory", configured},
            {"defaultDirectory", defaultCache.string()}};
}
void State::cacheAction(const std::string &operation) {
    auto s = config();
    std::lock_guard l(cacheJobMutex);
    if (operation == "cancel") {
        cancelCache = true;
        pendingCache.clear();
        if (cacheJob["running"] == true)
            cacheJob["phase"] = "stopping";
        return;
    }
    require(operation == "clear" || operation == "rebuild", "未知缓存操作");
    require(cacheJob["running"] == false, "已有缓存任务正在运行");
    if (operation == "rebuild")
        require(fs::is_directory(s["source"].get<std::string>()), "图库目录不存在");
    cancelCache = false;
    cacheJob["running"] = true;
    cacheJob["phase"] = "clearing";
    try {
        spawn([this, operation, root = fs::path(s["source"].get<std::string>())] {
            cacheWorker(operation, root);
        });
    } catch (...) {
        cacheJob["running"] = false;
        throw;
    }
}
void State::warmLibrary() {
    auto s = config();
    std::lock_guard l(cacheJobMutex);
    if (cacheJob["running"] == true) {
        pendingCache = s["source"];
        cancelCache = true;
        cacheJob["phase"] = "stopping";
        return;
    }
    cancelCache = false;
    cacheJob["running"] = true;
    try {
        spawn([this, root = fs::path(s["source"].get<std::string>())] { cacheWorker("warmup", root); });
    } catch (const std::exception &e) {
        cacheJob.update({{"running", false}, {"phase", "failed"}, {"error", e.what()}});
    }
}
void State::cacheWorker(std::string operation, fs::path root) {
    auto update = [this](const J &v) {
        std::lock_guard l(cacheJobMutex);
        cacheJob.update(v);
    };
    while (!stopping) {
        update({{"running", true},
                {"operation", operation},
                {"phase", operation == "warmup" ? "scanning" : "clearing"},
                {"source", root.string()},
                {"processed", 0},
                {"failed", 0},
                {"total", 0},
                {"current", ""},
                {"error", ""}});
        try {
            std::shared_ptr<FitsCache> c;
            {
                std::lock_guard l(mutex);
                c = cache;
            }
            auto stop = [&] { return stopping || cancelCache || config()["source"] != root.string(); };
            if (stop())
                update({{"phase", "cancelled"}});
            else {
                if (operation != "clear")
                    require(fs::is_directory(root), "图库目录不存在，连接磁盘后可手动重建缓存");
                if (operation == "clear" || operation == "rebuild")
                    c->clear();
                if (operation != "clear") {
                    update({{"phase", "scanning"}});
                    auto paths = mediaFiles(root, stop, false);
                    paths.erase(std::remove_if(paths.begin(), paths.end(), [](auto &p) { return !raw(p); }),
                                paths.end());
                    update({{"phase", "building"}, {"total", paths.size()}});
                    int processed = 0, failed = 0;
                    for (auto &p : paths) {
                        if (stop())
                            break;
                        update({{"current", p.lexically_relative(root).string()}});
                        try {
                            regularStamp(root, p.lexically_relative(root));
                            c->header(p);
                            c->preview(p);
                            auto info = c->info();
                            require(info["error"] == "", info["error"]);
                        } catch (const std::exception &e) {
                            failed++;
                            update({{"failed", failed}, {"error", p.filename().string() + "：" + e.what()}});
                        }
                        update({{"processed", ++processed}});
                    }
                }
                update({{"phase", stop() ? "cancelled" : "completed"}});
            }
        } catch (const std::exception &e) {
            update({{"phase", "failed"}, {"error", e.what()}});
        }
        std::lock_guard l(cacheJobMutex);
        if (pendingCache.empty() || stopping) {
            cacheJob["running"] = false;
            cacheJob["current"] = "";
            return;
        }
        root = pendingCache;
        pendingCache.clear();
        operation = "warmup";
        cancelCache = false;
    }
    update({{"running", false}, {"phase", "cancelled"}});
}
void State::cacheDirectory(const J &directory) {
    require(directory.is_string(), "缓存目录必须为文本");
    std::string input = trim(directory);
    fs::path root;
    if (input.empty())
        root = defaultCache;
    else {
        auto parent = normalized(input);
        require(fs::is_directory(parent), "请选择已存在的缓存目录");
        root = parent / "AstroLibrary-FIT-Cache";
    }
    auto candidate = std::make_shared<FitsCache>(root);
    auto info = candidate->info();
    require(info["error"] == "", info["error"]);
    std::lock_guard jobLock(cacheJobMutex);
    require(cacheJob["running"] == false, "请先停止缓存任务再切换目录");
    std::lock_guard l(mutex);
    J next = settings;
    next["fitsCacheDirectory"] = input.empty() ? "" : root.string();
    writeJSON(dataDir / "settings.json", next);
    settings = next;
    cache = candidate;
}
void State::startImport() {
    std::lock_guard l(mutex);
    fs::path source = settings["seestarSource"].get<std::string>(),
             destination = settings["source"].get<std::string>();
    require(!source.empty(), "请先选择 Seestar 同步目录");
    validateRoots(source, destination);
    require(fs::is_directory(destination), "图库目录不存在");
    bool expected = false;
    require(busy.compare_exchange_strong(expected, true), "已有导入、导出或清理任务正在运行");
    importStatus = {{"running", true},
                    {"source", source.string()},
                    {"destination", destination.string()},
                    {"imported", 0},
                    {"skipped", 0},
                    {"current", ""},
                    {"error", ""},
                    {"finished", false}};
    try {
        spawn([this, source, destination] {
            try {
                auto roots = J::array({rootStamp(source), rootStamp(destination)});
                for (auto &p : mediaFiles(source)) {
                    if (stopping)
                        break;
                    require(rootStamp(source) == roots[0] && rootStamp(destination) == roots[1],
                            "目录已更改，导入已停止");
                    auto rel = p.lexically_relative(source);
                    regularStamp(source, rel);
                    {
                        std::lock_guard l(mutex);
                        importStatus["current"] = rel.string();
                    }
                    bool copied = copyFile(p, destination / rel);
                    std::lock_guard l(mutex);
                    auto key = copied ? "imported" : "skipped";
                    importStatus[key] = importStatus[key].get<int>() + 1;
                }
            } catch (const std::exception &e) {
                std::lock_guard l(mutex);
                importStatus["error"] = e.what();
            }
            invalidate();
            {
                std::lock_guard l(mutex);
                importStatus["running"] = false;
                importStatus["finished"] = true;
                importStatus["current"] = "";
                busy = false;
            }
        });
    } catch (...) {
        busy = false;
        importStatus["running"] = false;
        throw;
    }
}
J State::exportFiles(const J &p) {
    J s;
    {
        std::lock_guard l(mutex);
        s = settings;
        bool expected = false;
        require(busy.compare_exchange_strong(expected, true), "已有导入、导出或清理任务正在运行");
    }
    struct Guard {
        std::atomic<bool> &b;
        ~Guard() {
            b = false;
        }
    } guard{busy};
    auto mode = p.value("mode", std::string("jpg"));
    require(mode == "jpg" || mode == "all", "导出模式无效");
    fs::path source = s["source"].get<std::string>(), dest = s["destination"].get<std::string>();
    validateRoots(source, dest);
    int found = 0, exported = 0, skipped = 0;
    for (auto &path : mediaFiles(source)) {
        if (stopping)
            break;
        auto rel = path.lexically_relative(source);
        if (mode == "jpg" && raw(path))
            continue;
        if (s["exportScope"] == "single" && mediaTarget(rel, false).second)
            continue;
        found++;
        if (copyFile(path, dest / rel))
            exported++;
        else
            skipped++;
    }
    return {{"ok", true},
            {"output", "找到 " + std::to_string(found) + " 个文件，导出 " + std::to_string(exported) +
                           "，跳过 " + std::to_string(skipped) + "。\n" + dest.string()}};
}
J State::cleanupAction(const std::string &kind, const std::string &operation, const J &p) {
    Cleanup &job = kind == "target" ? targetCleanup : kind == "seestar" ? seestarCleanup : jpegCleanup;
    J context;
    {
        std::lock_guard stateLock(mutex);
        if (operation == "cancel") {
            std::lock_guard l(job.mutex);
            if (kind == "target")
                require(p.value("jobId", J()) == job.status["jobId"], "清理任务已更改");
            job.cancelled = true;
            job.plan = nullptr;
            job.status["token"] = "";
            job.status["phase"] = job.status["running"] == true ? "stopping" : "cancelled";
            J result = job.status;
            result["ok"] = true;
            return result;
        }
        require(operation == "preview" || operation == "execute", "未知清理操作");
        std::string source = settings[kind == "seestar" ? "seestarSource" : "source"],
                    dest = kind == "seestar" ? settings["source"].get<std::string>() : "";
        require(!source.empty() && fs::is_directory(source), "照片目录不存在");
        context = {{"source", source},
                   {"destination", dest},
                   {"mergeTargets", settings["mergeTargets"]},
                   {"target", ""}};
        if (kind == "seestar") {
            validateRoots(source, dest);
            require(fs::is_directory(dest), "图库目录不存在");
        }
        if (kind == "target") {
            require(p.value("source", J()) == settings["source"] &&
                        p.value("mergeTargets", J()) == settings["mergeTargets"],
                    "图库目录或合并设置已更改，请刷新后重新预览");
            require(p.value("target", J()).is_string(), "目标必须为文本");
            auto target = trim(p["target"]);
            require(!target.empty() && target.size() <= 2048, "目标不能为空或过长");
            context["target"] = target;
        }
        bool expected = false;
        require(busy.compare_exchange_strong(expected, true), "已有导入、导出或清理任务正在运行");
        try {
            std::lock_guard l(job.mutex);
            if (operation == "execute") {
                require(!job.plan.is_null() && now() - job.plannedAt < 600 &&
                            p.value("token", J()) == job.status["token"] && job.status["token"] != "",
                        "清理预览已过期，请重新确认");
                if (kind == "target")
                    require(p.value("jobId", J()) == job.status["jobId"], "清理任务已更改");
                require(job.plan["context"] == context, "清理目录或目标已更改，请重新预览");
                require(rootStamp(source) == job.plan["rootStamp"], "源目录已更改");
                if (kind == "seestar")
                    require(rootStamp(dest) == job.plan["destStamp"], "图库目录已更改");
                context["files"] = job.plan["files"];
                context["rootStamp"] = job.plan["rootStamp"];
                context["destStamp"] = job.plan["destStamp"];
                job.plan = nullptr;
                job.status["token"] = "";
                job.status["phase"] = "cleaning";
                job.status["moved"] = 0;
                job.status["failed"] = 0;
            } else {
                job.plan = nullptr;
                Cleanup empty;
                job.status = empty.status;
                job.status.update(context);
                job.status["jobId"] = randomToken();
                job.status["phase"] = kind == "seestar" ? "checking" : "scanning";
            }
            job.status["running"] = true;
            job.cancelled = false;
        } catch (...) {
            busy = false;
            throw;
        }
    }
    bool execute = operation == "execute";
    if (kind == "jpg")
        cleanupWorker(job, kind, context, execute);
    else
        try {
            spawn([this, &job, kind, context, execute] { cleanupWorker(job, kind, context, execute); });
        } catch (...) {
            busy = false;
            job.update({{"running", false}, {"phase", "failed"}, {"error", "无法启动清理任务"}});
            throw;
        }
    J result = job.snapshot();
    result["ok"] = true;
    return result;
}
void State::cleanupWorker(Cleanup &job, std::string kind, J context, bool execute) {
    fs::path root = context["source"].get<std::string>(), dest = context["destination"].get<std::string>();
    auto stop = [&] { return stopping || job.cancelled.load(); };
    try {
        if (!execute) {
            auto identity = rootStamp(root);
            J destIdentity = kind == "seestar" ? rootStamp(dest) : J();
            J files = J::array();
            int checked = 0, skipped = 0, masters = 0, subs = 0;
            int64_t bytes = 0;
            for (auto &path : mediaFiles(root, stop)) {
                if (stop())
                    break;
                auto rel = path.lexically_relative(root);
                auto [target, sub] = mediaTarget(rel, context["mergeTargets"]);
                if (kind == "target" && target != context["target"])
                    continue;
                if (kind == "jpg" && lower(path.extension()) != ".jpg" && lower(path.extension()) != ".jpeg")
                    continue;
                job.update({{"current", rel.string()}});
                try {
                    auto first = regularStamp(root, rel);
                    J second;
                    if (kind == "seestar") {
                        second = regularStamp(dest, rel);
                        require(first[2] == second[2] && equalFiles(path, dest / rel) &&
                                    first == regularStamp(root, rel) && second == regularStamp(dest, rel),
                                "未找到完整一致的副本");
                    }
                    files.push_back({{"relative", rel.string()}, {"stamp", first}, {"destStamp", second}});
                    bytes += first[2].get<int64_t>();
                    if (sub)
                        subs++;
                    else
                        masters++;
                } catch (...) {
                    skipped++;
                }
                job.update({{"checked", ++checked},
                            {"count", files.size()},
                            {"bytes", bytes},
                            {"skipped", skipped},
                            {"masters", masters},
                            {"subs", subs}});
            }
            require(rootStamp(root) == identity && (kind != "seestar" || rootStamp(dest) == destIdentity),
                    "目录已变化，请重新预览");
            std::lock_guard l(job.mutex);
            if (!stop()) {
                job.plan = {{"context", context},
                            {"files", files},
                            {"rootStamp", identity},
                            {"destStamp", destIdentity}};
                job.plannedAt = now();
                job.status["token"] = randomToken();
                job.status["phase"] = "ready";
            }
        } else {
            int moved = 0, failed = 0, skipped = job.snapshot()["skipped"];
            for (auto &file : context["files"]) {
                if (stop())
                    break;
                require(rootStamp(root) == context["rootStamp"] &&
                            (kind != "seestar" || rootStamp(dest) == context["destStamp"]),
                        "目录已变化，清理已停止");
                fs::path rel = file["relative"].get<std::string>();
                job.update({{"current", rel.string()}});
                bool same = false;
                try {
                    same = regularStamp(root, rel) == file["stamp"] &&
                           (kind != "seestar" || regularStamp(dest, rel) == file["destStamp"]);
                } catch (...) {
                }
                if (!same)
                    skipped++;
                else
                    try {
                        platformTrash(root / rel);
                        moved++;
                    } catch (const std::exception &e) {
                        failed++;
                        std::lock_guard l(job.mutex);
                        job.status["error"] = e.what();
                        if (job.status["errors"].size() < 100)
                            job.status["errors"].push_back({{"path", rel.string()}, {"error", e.what()}});
                    }
                job.update({{"moved", moved}, {"skipped", skipped}, {"failed", failed}});
            }
            job.update({{"phase", stop() ? "cancelled" : "completed"}});
        }
    } catch (const std::exception &e) {
        job.update({{"phase", "failed"}, {"error", e.what()}, {"token", ""}});
    }
    if (stop())
        job.update({{"phase", "cancelled"}, {"token", ""}});
    if (execute)
        invalidate();
    job.update({{"running", false}, {"current", ""}});
    busy = false;
}
bool State::syncR2(bool queue) {
    std::lock_guard l(mutex);
    require(settings["r2"]["enabled"] == true && r2Public(settings["r2"])["configured"] == true,
            "请先保存并启用完整的 R2 配置");
    if (r2Status["syncing"] == true) {
        if (queue)
            r2Queued = true;
        return false;
    }
    r2Status.update({{"syncing", true},
                     {"pending", 0},
                     {"uploaded", 0},
                     {"skipped", 0},
                     {"failed", 0},
                     {"lastError", ""}});
    try {
        spawn([this, c = settings["r2"], gen = r2Generation, roots = settings] { r2Worker(c, gen, roots); });
    } catch (...) {
        r2Status["syncing"] = false;
        throw;
    }
    return true;
}
void State::r2Worker(J c, uint64_t generation, J roots) {
    int uploaded = 0, skipped = 0, failed = 0;
    std::string error;
    auto stop = [&] {
        std::lock_guard l(mutex);
        return stopping || generation != r2Generation;
    };
    try {
        auto p = library(false, true);
        J items = p["items"];
        for (auto &item : p["editedItems"])
            items.push_back(item);
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [](auto &i) { return i["kind"] != "photo" || i.value("isSub", false); }),
                    items.end());
        for (auto &item : items) {
            if (stop())
                break;
            try {
                std::string id = item["id"];
                bool edited = item.value("isEdited", false);
                auto source = safePath(roots[edited ? "editedSource" : "source"].get<std::string>(),
                                       edited ? id.substr(7) : id);
                auto st = stamp(source);
                auto fingerprint =
                    sha256(source.string() + std::string(1, '\0') + std::to_string(st[2].get<int64_t>()) +
                           std::string(1, '\0') + std::to_string(st[3].get<int64_t>()) +
                           std::string(1, '\0') + "1600" + std::string(1, '\0') + "82");
                auto stem = std::regex_replace(source.stem().string(), std::regex("[^A-Za-z0-9._-]+"), "-");
                auto a = stem.find_first_not_of("-._"), b = stem.find_last_not_of("-._");
                stem = a == stem.npos ? "preview" : stem.substr(a, b - a + 1).substr(0, 64);
                auto key = c["keyPrefix"].get<std::string>() + "/" + (edited ? "edited" : "source") + "/" +
                           sha256(id).substr(0, 24) + "-" + stem + ".jpg";
                auto uri = c["publicBaseUrl"] == ""
                               ? ""
                               : c["publicBaseUrl"].get<std::string>() + "/" + urlEncode(key);
                J existing;
                {
                    std::lock_guard l(mutex);
                    existing = mapping["items"].value(id, J::object());
                }
                if (existing.value("fingerprint", std::string()) == fingerprint &&
                    existing.value("accountId", J()) == c["accountId"] &&
                    existing.value("bucket", J()) == c["bucket"] &&
                    existing.value("objectKey", std::string()) == key) {
                    if (existing.value("publicUri", std::string()) != uri) {
                        std::lock_guard l(mutex);
                        if (generation == r2Generation) {
                            auto next = mapping;
                            next["items"][id]["publicUri"] = uri;
                            writeJSON(dataDir / "r2-mappings.json", next);
                            mapping = next;
                        }
                    }
                    skipped++;
                } else {
                    auto bytes = platformJPEG(source);
                    require(stamp(source) == st, "源图正在变化");
                    if (stop())
                        break;
                    auto response = r2Request(c, "PUT", key, bytes);
                    J record = {{"itemId", id},
                                {"sourceKind", edited ? "edited" : "source"},
                                {"fingerprint", fingerprint},
                                {"accountId", c["accountId"]},
                                {"bucket", c["bucket"]},
                                {"objectKey", key},
                                {"remoteUri", "r2://" + c["bucket"].get<std::string>() + "/" + key},
                                {"publicUri", uri},
                                {"etag", response["headers"].value("etag", std::string())},
                                {"previewBytes", bytes.size()},
                                {"uploadedAt", isoTime(now())}};
                    std::lock_guard l(mutex);
                    if (generation != r2Generation)
                        break;
                    auto next = mapping;
                    next["items"][id] = record;
                    writeJSON(dataDir / "r2-mappings.json", next);
                    mapping = next;
                    uploaded++;
                }
            } catch (const std::exception &e) {
                failed++;
                error = e.what();
            }
            std::lock_guard l(mutex);
            r2Status.update({{"pending", std::max(0, int(items.size()) - uploaded - skipped - failed)},
                             {"uploaded", uploaded},
                             {"skipped", skipped},
                             {"failed", failed},
                             {"lastError", error}});
        }
    } catch (const std::exception &e) {
        failed++;
        error = e.what();
    }
    bool restart;
    {
        std::lock_guard l(mutex);
        restart = r2Queued && !stopping;
        r2Queued = false;
        r2Status.update(
            {{"syncing", false},
             {"pending", 0},
             {"uploaded", uploaded},
             {"skipped", skipped},
             {"failed", failed},
             {"lastSyncAt", isoTime(now())},
             {"lastError", generation != r2Generation ? "R2 配置已变更，旧同步任务已停止" : error}});
    }
    if (restart)
        try {
            syncR2();
        } catch (...) {
        }
}
} // namespace astro
