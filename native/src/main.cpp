#include "state.hpp"
#include <iostream>
#include <mach-o/dyld.h>
#include <sqlite3.h>
#include <unistd.h>
#include <zlib.h>
namespace astro {
static void selfTest() {
    require(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA256 self-test failed");
    const auto head =
        sigv4("HEAD", std::string(32, 'a') + ".r2.cloudflarestorage.com", "/astro-bucket/preview.jpg", "",
              std::string(20, 'A'), "example-secret", "20260910T000000Z");
    const auto put =
        sigv4("PUT", std::string(32, 'a') + ".r2.cloudflarestorage.com", "/astro-bucket/preview.jpg",
              "example", std::string(20, 'A'), "example-secret", "20260910T000000Z");
    require(head.at("Authorization")
                        .get<std::string>()
                        .find("Signature=584261195dd29da4e441c0856d577d4a4a1d0a8a74645fa31bab03f511df40a8") !=
                    std::string::npos &&
                put.at("Authorization")
                        .get<std::string>()
                        .find("Signature=d659e4d05e71149c9f47dffc0b67d20993863a3a32a2d27586bfe035f1e7f2ad") !=
                    std::string::npos,
            "R2 SigV4 compatibility self-test failed");
    sqlite3 *db = nullptr;
    require(sqlite3_open(":memory:", &db) == SQLITE_OK, "SQLite unavailable");
    int rc =
        sqlite3_exec(db, "CREATE TABLE probe(value);INSERT INTO probe VALUES(1);", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(rc == SQLITE_OK, "SQLite self-test failed");
    Sample s;
    s.w = 2;
    s.h = 2;
    s.c = 1;
    s.bitpix = 8;
    s.header = J::object();
    s.values = {0, 64, 128, 255};
    Options o;
    o.mode = "linear";
    require(renderSample(s, o).substr(1, 3) == "PNG", "PNG self-test failed");
    std::cout << J{{"ok", true},       {"protocolVersion", 1},           {"version", "0.8.0"},
                   {"backend", "cpp"}, {"sqlite", sqlite3_libversion()}, {"zlib", zlibVersion()}}
                     .dump()
              << std::endl;
}
} // namespace astro
int main(int argc, char **argv) {
    using namespace astro;
    try {
        std::string command = argc > 1 ? argv[1] : "--help";
        if (command == "--self-test") {
            selfTest();
            return 0;
        }
        if (command == "--version") {
            std::cout << "AstroLibrary 0.8.0 (C++17 native)\n";
            return 0;
        }
        if (command == "--help" || command == "-h") {
            std::cout << "AstroLibrary — native CLI + macOS App\n  serve [--library DIR] [--port PORT] "
                         "[--open-browser]\n  export [-s DIR] [-d DIR] [--jpg-only] [--single-only] "
                         "[--overwrite] [--dry-run] [--quiet]\n  auth [--disable] [--username NAME]\n  "
                         "render --input FILE.fit --output preview.png [--mode auto|stretch|linear] [--black "
                         "-3..3] [--brightness -2..2] [--neutralize]\n  --self-test | "
                         "--version\nDevelopment: --data-dir DIR --cache-dir DIR --web-root DIR\n";
            return 0;
        }
        std::map<std::string, std::string> args;
        std::set<std::string> flags;
        std::set<std::string> bools = {"--open-browser", "--jpg-only", "--single-only", "--overwrite",
                                       "--dry-run",      "--quiet",    "--disable",     "--neutralize"};
        std::set<std::string> values = {"--library",   "--source",   "-s",         "--destination",
                                        "-d",          "--host",     "--port",     "--data-dir",
                                        "--cache-dir", "--web-root", "--username", "--input",
                                        "--output",    "--mode",     "--black",    "--brightness"};
        for (int i = 2; i < argc; i++) {
            std::string k = argv[i];
            if (bools.count(k))
                flags.insert(k);
            else {
                require(values.count(k), "未知参数：" + k);
                require(i + 1 < argc, "缺少参数：" + k);
                args[k] = argv[++i];
            }
        }
        if (command == "render") {
            require(args.count("--input") && args.count("--output"), "请提供 --input 和 --output");
            Options o;
            if (args.count("--mode"))
                o.mode = args["--mode"];
            if (args.count("--black"))
                o.black = std::stod(args["--black"]);
            if (args.count("--brightness"))
                o.brightness = std::stod(args["--brightness"]);
            o.neutralize = flags.count("--neutralize");
            auto in = normalized(args["--input"]), out = normalized(args["--output"]);
            require(in != out && !fs::exists(out), "预览输出不能覆盖现有文件");
            double start = now();
            auto sample = sampleFits(in);
            double decoded = now();
            auto bytes = renderSample(sample, o);
            atomicWrite(out, bytes);
            std::cout << J{{"ok", true},
                           {"width", sample.w},
                           {"height", sample.h},
                           {"channels", sample.c},
                           {"decodeMs", (decoded - start) * 1000},
                           {"totalMs", (now() - start) * 1000},
                           {"output", out.string()}}
                             .dump()
                      << std::endl;
            return 0;
        }
        require(command == "serve" || command == "export" || command == "auth", "未知命令：" + command);
        fs::path data = args.count("--data-dir") ? fs::path(args["--data-dir"])
                                                 : home() / "Library/Application Support/AstroLibrary";
        J overrides = J::object();
        for (auto k : {"--library", "--source", "-s"})
            if (args.count(k))
                overrides["source"] = args[k];
        for (auto k : {"--destination", "-d"})
            if (args.count(k))
                overrides["destination"] = args[k];
        if (args.count("--port"))
            overrides["port"] = std::stoi(args["--port"]);
        if (args.count("--cache-dir"))
            overrides["cacheDir"] = args["--cache-dir"];
        fs::path web;
        if (args.count("--web-root"))
            web = args["--web-root"];
        else {
            uint32_t n = 0;
            _NSGetExecutablePath(nullptr, &n);
            std::string b(n, 0);
            _NSGetExecutablePath(b.data(), &n);
            auto exe = normalized(b.c_str());
            for (auto p :
                 {exe.parent_path() / "web", exe.parent_path().parent_path() / "Resources/AstroLibrary/web",
                  exe.parent_path().parent_path().parent_path() / "web", fs::current_path() / "web"})
                if (fs::is_regular_file(p / "index.html")) {
                    web = p;
                    break;
                }
            if (web.empty())
                web = fs::current_path() / "web";
        }
        if (command == "export") {
            auto saved = readJSON(normalized(data) / "settings.json");
            auto source = normalized(overrides.value(
                "source", saved.value("libraryPath",
                                      saved.value("source", (home() / "Pictures/AstroLibrary").string()))));
            auto dest = normalized(overrides.value(
                "destination",
                saved.value("destination", (home() / "Pictures/Seestar导出/MyWorks").string())));
            validateRoots(source, dest);
            int found = 0, exported = 0, skipped = 0;
            for (auto &p : mediaFiles(source)) {
                auto rel = p.lexically_relative(source);
                if (flags.count("--jpg-only") && raw(p))
                    continue;
                if (flags.count("--single-only") && mediaTarget(rel, false).second)
                    continue;
                found++;
                if (flags.count("--dry-run")) {
                    if (!flags.count("--quiet"))
                        std::cout << "[预览] " << p << " -> " << dest / rel << '\n';
                    continue;
                }
                if (copyFile(p, dest / rel, flags.count("--overwrite")))
                    exported++;
                else
                    skipped++;
            }
            std::cout << J{{"found", found},
                           {"exported", exported},
                           {"skipped", skipped},
                           {"dryRun", bool(flags.count("--dry-run"))},
                           {"destination", dest.string()}}
                             .dump()
                      << std::endl;
            return 0;
        }
        State state(data, web, overrides);
        if (command == "auth") {
            if (flags.count("--disable"))
                state.setAuth({{"enabled", false}});
            else {
                std::string name = args["--username"];
                if (name.empty()) {
                    std::cout << "账号名: ";
                    std::getline(std::cin, name);
                }
                char *first = getpass("新密码（8–1024 字符）: ");
                require(first, "无法读取密码");
                std::string password = first;
                char *second = getpass("再次输入密码: ");
                require(second && password == second, "两次密码不一致");
                state.setAuth({{"enabled", true}, {"username", name}, {"password", password}});
            }
            std::cout << "账号保护配置已保存，重启 AstroLibrary 后生效。\n";
            return 0;
        }
        for (auto file : {"index.html", "app.js", "styles.css"})
            require(fs::is_regular_file(web / file), "缺少 Web 资源：" + (web / file).string());
        int port = state.config()["port"];
        if (flags.count("--open-browser")) {
            std::string url = "http://127.0.0.1:" + std::to_string(port);
            state.spawn([url] {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                platformBrowse(url);
            });
        }
        return runServer(state, args.count("--host") ? args["--host"] : "127.0.0.1", port);
    } catch (const std::exception &e) {
        std::cerr << "AstroLibrary: " << e.what() << std::endl;
        return 1;
    }
}
