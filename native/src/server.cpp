#include "httplib.h"
#include "state.hpp"
#include <arpa/inet.h>
#include <csignal>
#include <regex>
namespace astro {
using Request = httplib::Request;
using Response = httplib::Response;
static volatile sig_atomic_t interrupted = 0;
static void interrupt(int) {
    interrupted = 1;
}
static void sendJSON(Response &r, const J &j, int status = 200) {
    r.status = status;
    r.set_content(j.dump(), "application/json; charset=utf-8");
    r.set_header("Cache-Control", "no-store");
}
static bool loopback(const std::string &s) {
    if (lower(s) == "localhost" || s == "::1")
        return true;
    in_addr a{};
    return inet_pton(AF_INET, s.c_str(), &a) == 1 && (ntohl(a.s_addr) >> 24) == 127;
}
static bool validHost(const std::string &value, int port) {
    std::smatch m;
    static std::regex r("^(\\[[0-9a-fA-F:]+\\]|[A-Za-z0-9.-]+)(:([0-9]{1,5}))?$");
    if (!std::regex_match(value, m, r))
        return false;
    std::string host = m[1];
    if (host.front() == '[')
        host = host.substr(1, host.size() - 2);
    if (!loopback(host))
        return false;
    return !m[3].matched || std::stoi(m[3]) == port;
}
static std::string cookie(const Request &r) {
    std::istringstream stream(r.get_header_value("Cookie"));
    std::string part;
    while (std::getline(stream, part, ';')) {
        part = trim(part);
        if (part.rfind("astrolibrary_session=", 0) == 0)
            return part.substr(std::string("astrolibrary_session=").size());
    }
    return "";
}
static std::string mime(const fs::path &p) {
    auto e = lower(p.extension());
    if (e == ".html")
        return "text/html; charset=utf-8";
    if (e == ".js")
        return "text/javascript; charset=utf-8";
    if (e == ".css")
        return "text/css; charset=utf-8";
    if (e == ".jpg" || e == ".jpeg")
        return "image/jpeg";
    if (e == ".png")
        return "image/png";
    if (e == ".svg")
        return "image/svg+xml";
    if (e == ".tif" || e == ".tiff")
        return "image/tiff";
    if (raw(p))
        return "application/fits";
    return "application/octet-stream";
}
int runServer(State &state, const std::string &host, int port) {
    require(loopback(host), "本地服务仅允许监听回环地址");
    require(port >= 1024 && port <= 65535, "端口必须介于 1024 和 65535");
    httplib::Server server;
    server.new_task_queue = [] { return new httplib::ThreadPool(8, 16, 32); };
    server.set_read_timeout(15, 0);
    server.set_write_timeout(15, 0);
    server.set_payload_max_length(128 * 1024);
    server.set_keep_alive_max_count(32);
    server.set_keep_alive_timeout(5);
    auto native = [&](const Request &r) {
        return state.nativeToken(r.get_header_value("X-AstroLibrary-App-Token"));
    };
    auto authStatus = [&](const Request &r) {
        auto s = state.config();
        bool enabled = s["auth"].value("enabled", false), authed = enabled && state.authenticated(cookie(r));
        return J{{"enabled", enabled},
                 {"authenticated", authed},
                 {"username", authed ? s["auth"].value("username", std::string()) : ""},
                 {"csrfToken", state.csrf}};
    };
    auto context = [&](const Request &r, Response &response) {
        auto denied = [&](int code, const char *message) {
            sendJSON(response, {{"ok", false}, {"error", message}}, code);
            return httplib::Server::HandlerResponse::Handled;
        };
        if (r.get_header_value_count("Host") != 1 || !validHost(r.get_header_value("Host"), port))
            return denied(421, "拒绝未知 Host");
        if (r.has_header("Transfer-Encoding"))
            return denied(400, "不支持 Transfer-Encoding");
        if (r.method != "GET" && r.method != "HEAD" && !native(r)) {
            auto origin = r.get_header_value("Origin");
            if (!origin.empty() && origin != "http://127.0.0.1:" + std::to_string(port) &&
                origin != "http://localhost:" + std::to_string(port) &&
                origin != "http://[::1]:" + std::to_string(port))
                return denied(403, "拒绝跨来源请求");
            if (!constantEqual(r.get_header_value("X-AstroLibrary-CSRF"), state.csrf))
                return denied(403, "CSRF 校验失败");
        }
        bool publicPath = r.path == "/api/health" || r.path == "/api/auth/status" ||
                          r.path == "/api/auth/login" || r.path == "/api/auth/logout" || r.path == "/" ||
                          r.path == "/index.html";
        bool protectedPath = r.path.rfind("/api/", 0) == 0 || r.path.rfind("/media/", 0) == 0 ||
                             r.path.rfind("/edited-media/", 0) == 0;
        if (protectedPath && !publicPath && !native(r) && !state.authenticated(cookie(r)))
            return denied(401, "需要登录后访问");
        return httplib::Server::HandlerResponse::Unhandled;
    };
    server.set_pre_routing_handler(context);
    server.set_pre_request_handler(context);
    server.set_post_routing_handler([](const Request &, Response &r) {
        r.set_header("Server", "AstroLibrary/0.8-native");
        r.set_header("X-Content-Type-Options", "nosniff");
        r.set_header("X-Frame-Options", "DENY");
        r.set_header("Referrer-Policy", "no-referrer");
        r.set_header("Cross-Origin-Resource-Policy", "same-origin");
        r.set_header("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
        r.set_header("Content-Security-Policy",
                     "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; "
                     "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
        if (!r.has_header("Cache-Control"))
            r.set_header("Cache-Control", "no-cache");
    });
    server.set_exception_handler([](const Request &, Response &r, std::exception_ptr exception) {
        std::string error = "服务发生错误";
        try {
            if (exception)
                std::rethrow_exception(exception);
        } catch (const std::exception &e) {
            error = e.what();
        } catch (...) {
        }
        sendJSON(r, {{"ok", false}, {"error", error}}, 400);
    });
    server.Get(R"(/.*)", [&](const Request &r, Response &response) {
        const auto &path = r.path;
        if (path == "/api/health" || path == "/api/app/health") {
            if (path == "/api/app/health" && !native(r)) {
                sendJSON(response, {{"ok", false}, {"error", "仅允许 AstroLibrary App 访问"}}, 403);
                return;
            }
            sendJSON(response,
                     {{"ok", true}, {"backend", "cpp"}, {"protocolVersion", 1}, {"version", "0.8.0"}});
            return;
        }
        if (path == "/api/auth/status") {
            sendJSON(response, authStatus(r));
            return;
        }
        if (path == "/api/settings") {
            sendJSON(response, state.settingsPayload());
            return;
        }
        if (path == "/api/library") {
            sendJSON(response, state.library(r.get_param_value("refresh") == "1"));
            return;
        }
        if (path == "/api/sub-collection") {
            try {
                sendJSON(response, state.subCollection(r.get_param_value("target")));
            } catch (const std::exception &e) {
                sendJSON(response, {{"ok", false}, {"error", e.what()}}, 404);
            }
            return;
        }
        if (path == "/api/fits-cache") {
            sendJSON(response, state.cacheStatus());
            return;
        }
        if (path == "/api/seestar/import") {
            std::lock_guard l(state.mutex);
            J j = state.importStatus;
            j["ok"] = true;
            sendJSON(response, j);
            return;
        }
        if (path == "/api/cleanup-target" || path == "/api/seestar/cleanup") {
            auto j = (path == "/api/cleanup-target" ? state.targetCleanup : state.seestarCleanup).snapshot();
            j["ok"] = true;
            sendJSON(response, j);
            return;
        }
        if (path.rfind("/api/fits-preview/", 0) == 0) {
            try {
                auto s = state.config();
                auto p = safePath(s["source"].get<std::string>(), path.substr(18));
                require(raw(p) && fs::is_regular_file(p), "未找到 FITS 图像");
                Options options;
                if (r.has_param("mode"))
                    options.mode = r.get_param_value("mode");
                auto numeric = [&](const char *key) {
                    if (!r.has_param(key))
                        return 0.0;
                    auto v = r.get_param_value(key);
                    size_t end;
                    double n = std::stod(v, &end);
                    require(end == v.size(), "预览参数无效");
                    return n;
                };
                options.black = numeric("black");
                options.brightness = numeric("brightness");
                if (r.has_param("neutralize")) {
                    auto value = r.get_param_value("neutralize");
                    require(value == "0" || value == "1", "背景中和参数无效");
                    options.neutralize = value == "1";
                }
                std::shared_ptr<FitsCache> cache;
                {
                    std::lock_guard l(state.mutex);
                    cache = state.cache;
                }
                auto etag = cache->etag(p, options);
                bool auth = s["auth"].value("enabled", false);
                response.set_header("ETag", etag);
                response.set_header("Cache-Control", auth ? "no-store" : "private, no-cache");
                if (!auth && r.get_header_value("If-None-Match") == etag) {
                    response.status = 304;
                    return;
                }
                response.set_content(cache->preview(p, options), "image/png");
            } catch (const std::exception &e) {
                sendJSON(response, {{"ok", false}, {"error", e.what()}}, 422);
            }
            return;
        }
        if (path == "/favicon.ico") {
            response.status = 204;
            return;
        }
        fs::path file;
        bool isMedia = false;
        auto s = state.config();
        try {
            if (path == "/" || path == "/index.html")
                file = state.webRoot / "index.html";
            else if (path.rfind("/assets/", 0) == 0)
                file = safePath(state.webRoot, path.substr(8));
            else if (path.rfind("/media/", 0) == 0) {
                file = safePath(s["source"].get<std::string>(), path.substr(7));
                isMedia = true;
            } else if (path.rfind("/edited-media/", 0) == 0) {
                file = safePath(s["editedSource"].get<std::string>(), path.substr(14));
                isMedia = true;
            } else
                throw std::runtime_error("Not found");
            require(fs::is_regular_file(file), "文件不存在");
            if (isMedia)
                require(media(file), "不是支持的照片文件");
            response.set_file_content(file.string(), mime(file));
            if (isMedia && s["auth"].value("enabled", false))
                response.set_header("Cache-Control", "no-store");
        } catch (const std::exception &e) {
            sendJSON(response, {{"ok", false}, {"error", e.what()}}, 404);
        }
    });
    server.Post(R"(/.*)", [&](const Request &r, Response &response) {
        const auto &path = r.path;
        J p = J::parse(r.body.empty() ? "{}" : r.body);
        require(p.is_object(), "请求内容必须是 JSON 对象");
        J result = {{"ok", true}};
        if (path == "/api/auth/status")
            result = authStatus(r);
        else if (path == "/api/auth/login") {
            auto token = state.login(p, r.remote_addr);
            response.set_header("Set-Cookie", "astrolibrary_session=" + token +
                                                  "; Max-Age=43200; Path=/; HttpOnly; SameSite=Strict");
            result["username"] = state.config()["auth"].value("username", std::string());
        } else if (path == "/api/auth/logout") {
            std::lock_guard l(state.mutex);
            state.sessions.erase(cookie(r));
            response.set_header("Set-Cookie",
                                "astrolibrary_session=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict");
        } else if (path == "/api/app/auth") {
            if (!native(r)) {
                sendJSON(response, {{"ok", false}, {"error", "仅允许 AstroLibrary App 修改账号保护"}}, 403);
                return;
            }
            state.setAuth(p);
            result.update(state.settingsPayload());
        } else if (path == "/api/settings") {
            state.updateSettings(p);
            result.update(state.settingsPayload());
        } else if (path == "/api/rating")
            result = state.rating(p);
        else if (path == "/api/workflow")
            result = state.workflow(p);
        else if (path == "/api/tags" || path == "/api/tags/remove")
            result = state.tags(p, path == "/api/tags/remove");
        else if (path == "/api/fits-cache/directory") {
            if (!native(r)) {
                sendJSON(response, {{"ok", false}, {"error", "请在本机 App 中选择缓存目录"}}, 403);
                return;
            }
            require(p.contains("directory"), "请提供缓存目录");
            state.cacheDirectory(p["directory"]);
        } else if (path.rfind("/api/fits-cache/", 0) == 0)
            state.cacheAction(path.substr(16));
        else if (path == "/api/seestar/detect") {
            auto found = detectSeestar();
            std::lock_guard l(state.mutex);
            state.candidates = found;
            result["candidates"] = found;
        } else if (path == "/api/seestar/import") {
            state.startImport();
            result["started"] = true;
        } else if (path == "/api/export")
            result = state.exportFiles(p);
        else if (path.rfind("/api/cleanup-target/", 0) == 0)
            result = state.cleanupAction("target", path.substr(20), p);
        else if (path.rfind("/api/seestar/cleanup/", 0) == 0)
            result = state.cleanupAction("seestar", path.substr(21), p);
        else if (path == "/api/cleanup-jpg/preview")
            result = state.cleanupAction("jpg", "preview", p);
        else if (path == "/api/cleanup-jpg")
            result = state.cleanupAction("jpg", "execute", p);
        else if (path == "/api/r2/test") {
            auto s = state.config();
            auto c = p.contains("r2") ? r2Config(p["r2"], s["r2"]["secretAccessKey"]) : s["r2"];
            r2Request(c, "HEAD");
            result["message"] = "R2 存储桶连接成功";
        } else if (path == "/api/r2/sync") {
            result["started"] = state.syncR2(true);
            result["status"] = state.r2StatusPayload();
        } else if (path == "/api/open-folder") {
            auto s = state.config();
            auto folder = safePath(s["source"].get<std::string>(), p.value("path", std::string()));
            auto name = lower(folder.filename());
            require(fs::is_directory(folder) && name.size() > 4 && name.substr(name.size() - 4) == "_sub",
                    "只能打开图库中的 SUB 文件夹");
            platformOpen(folder);
            result["path"] = folder.string();
        } else if (path == "/api/source/select" || path == "/api/destination/select" ||
                   path == "/api/edited/select") {
            std::string selected = p.value("path", std::string());
            if (selected.empty())
                selected = platformChooseDirectory();
            auto folder = normalized(selected);
            require(fs::is_directory(folder), "请选择有效目录");
            const char *key = path == "/api/source/select"        ? "libraryPath"
                              : path == "/api/destination/select" ? "destination"
                                                                  : "editedSource";
            state.updateSettings({{key, folder.string()}});
            result.update(state.settingsPayload());
            result["selected"] = folder.string();
        } else {
            sendJSON(response, {{"ok", false}, {"error", "Not found"}}, 404);
            return;
        }
        sendJSON(response, result);
    });
    require(server.bind_to_port(host, port), "无法监听端口 " + std::to_string(port) + "，可能已被占用");
    interrupted = 0;
    auto oldTerm = signal(SIGTERM, interrupt), oldInt = signal(SIGINT, interrupt);
    std::atomic<bool> ended{false};
    std::thread watcher([&] {
        while (!ended && !interrupted)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (interrupted) {
            state.stopping = true;
            server.stop();
        }
    });
    fprintf(stdout, "AstroLibrary C++ http://%s:%d\n", host.c_str(), port);
    fflush(stdout);
    bool ok = server.listen_after_bind();
    ended = true;
    watcher.join();
    signal(SIGTERM, oldTerm);
    signal(SIGINT, oldInt);
    return ok || interrupted ? 0 : 1;
}
} // namespace astro
