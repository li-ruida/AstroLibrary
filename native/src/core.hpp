#pragma once
#include "json.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace astro {
namespace fs = std::filesystem;
using J = nlohmann::json;
using Bytes = std::string;
inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::string trim(std::string s), lower(std::string s), sha256(const std::string &s), randomToken(),
    urlEncode(const std::string &s), urlDecode(const std::string &s);
std::string hmac(const std::string &key, const std::string &data), hex(const std::string &data);
size_t textLength(const std::string &text);
bool constantEqual(const std::string &a, const std::string &b);
J passwordHash(const std::string &password);
bool verifyPassword(const std::string &password, const J &record);
double now();
std::string isoTime(double seconds, bool zone = true), utcTime(const char *format);
fs::path home(), normalized(fs::path path), safePath(const fs::path &root, const std::string &relative);
bool inside(const fs::path &path, const fs::path &root), raw(const fs::path &path),
    photo(const fs::path &path), media(const fs::path &path);
J stamp(const fs::path &path), rootStamp(const fs::path &path),
    regularStamp(const fs::path &root, const fs::path &relative);
std::vector<fs::path> mediaFiles(const fs::path &root, const std::function<bool()> &cancel = {},
                                 bool includeSub = true);
std::pair<std::string, bool> mediaTarget(const fs::path &relative, bool merge);
std::string canonical(std::string name);
Bytes readBytes(const fs::path &path, size_t limit = 128 * 1024 * 1024);
J readJSON(const fs::path &path, J fallback = J::object());
void atomicWrite(const fs::path &path, const Bytes &bytes);
void writeJSON(const fs::path &path, const J &j);
bool copyFile(const fs::path &source, const fs::path &destination, bool overwrite = false);
void validateRoots(const fs::path &source, const fs::path &destination);
bool equalFiles(const fs::path &a, const fs::path &b);
J sourceStatus(const fs::path &path);
J detectSeestar();
std::string platformChooseDirectory();
void platformBrowse(const std::string &url);
void platformOpen(const fs::path &path);
void platformTrash(const fs::path &path);
J platformHTTPS(const std::string &method, const std::string &url, const J &headers, const Bytes &body);
Bytes platformJPEG(const fs::path &source);
struct Sample {
    int w = 0, h = 0, c = 0, bitpix = 0;
    J header;
    std::vector<double> values;
};
struct Options {
    std::string mode = "auto";
    double black = 0, brightness = 0;
    bool neutralize = false;
    J json() const {
        return J::array({mode, black, brightness, neutralize});
    }
    void validate();
};
J fitsHeader(const fs::path &path);
Sample sampleFits(const fs::path &path);
Bytes renderSample(const Sample &sample, const Options &options = {});
class FitsCache {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit FitsCache(fs::path root);
    ~FitsCache();
    J header(const fs::path &path);
    Bytes preview(const fs::path &path, Options options = {});
    J info();
    void clear();
    std::string etag(const fs::path &path, Options options = {});
};
J r2Config(J payload, const std::string &existingSecret = "");
J r2Public(const J &config);
J r2Request(const J &config, const std::string &method, const std::string &key = "", const Bytes &body = "");
J sigv4(const std::string &method, const std::string &host, const std::string &uri, const Bytes &body,
        const std::string &key, const std::string &secret, const std::string &date = "");
} // namespace astro
