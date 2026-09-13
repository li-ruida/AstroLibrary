#include "core.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
namespace astro {
std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \r\n\t");
    if (a == s.npos)
        return "";
    return s.substr(a, s.find_last_not_of(" \r\n\t") - a + 1);
}
std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return s;
}
std::string hex(const std::string &s) {
    const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
std::string sha256(const std::string &s) {
    unsigned char digest[32];
    CC_SHA256(s.data(), (CC_LONG)s.size(), digest);
    return hex(std::string((char *)digest, 32));
}
std::string hmac(const std::string &key, const std::string &data) {
    std::string out(32, 0);
    CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), data.data(), data.size(), out.data());
    return out;
}
std::string randomToken() {
    std::string bytes(32, 0);
    arc4random_buf(bytes.data(), bytes.size());
    return hex(bytes);
}
size_t textLength(const std::string &text) {
    return std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
}
bool constantEqual(const std::string &a, const std::string &b) {
    volatile unsigned int d = (unsigned)(a.size() ^ b.size());
    for (size_t i = 0; i < a.size(); i++)
        d |= (unsigned char)a[i] ^ (i < b.size() ? (unsigned char)b[i] : 0);
    return d == 0;
}
static std::string unhex(const std::string &s) {
    require(s.size() % 2 == 0, "Invalid hex");
    std::string out;
    for (size_t i = 0; i < s.size(); i += 2) {
        auto digit = [](char c) {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            throw std::runtime_error("Invalid hex");
        };
        out += char(digit(s[i]) * 16 + digit(s[i + 1]));
    }
    return out;
}
static std::string pbkdf(const std::string &p, const std::string &salt, int rounds) {
    std::string out(32, 0);
    require(CCKeyDerivationPBKDF(kCCPBKDF2, p.data(), p.size(), (const uint8_t *)salt.data(), salt.size(),
                                 kCCPRFHmacAlgSHA256, rounds, (uint8_t *)out.data(), 32) == 0,
            "密码计算失败");
    return hex(out);
}
J passwordHash(const std::string &p) {
    std::string salt(16, 0);
    arc4random_buf(salt.data(), salt.size());
    return {{"algorithm", "pbkdf2_sha256"},
            {"iterations", 600000},
            {"salt", hex(salt)},
            {"hash", pbkdf(p, salt, 600000)}};
}
bool verifyPassword(const std::string &p, const J &r) {
    try {
        int n = r.at("iterations");
        require(n > 0 && n <= 10000000 && r.at("algorithm") == "pbkdf2_sha256", "密码配置无效");
        return constantEqual(pbkdf(p, unhex(r.at("salt")), n), r.at("hash"));
    } catch (...) {
        return false;
    }
}
double now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string isoTime(double seconds, bool zone) {
    time_t t = (time_t)seconds;
    tm date{};
    localtime_r(&t, &date);
    char out[64];
    strftime(out, sizeof(out), zone ? "%Y-%m-%dT%H:%M:%S%z" : "%Y-%m-%dT%H:%M:%S", &date);
    std::string s = out;
    if (zone && s.size() == 24)
        s.insert(22, ":");
    return s;
}
std::string utcTime(const char *fmt) {
    time_t t = time(nullptr);
    tm date{};
    gmtime_r(&t, &date);
    char out[64];
    strftime(out, sizeof(out), fmt, &date);
    return out;
}
fs::path home() {
    const char *v = getenv("HOME");
    require(v && *v, "HOME 未设置");
    return v;
}
fs::path normalized(fs::path p) {
    auto s = p.string();
    if (s == "~")
        p = home();
    else if (s.rfind("~/", 0) == 0)
        p = home() / s.substr(2);
    return fs::weakly_canonical(fs::absolute(p));
}
bool inside(const fs::path &p, const fs::path &root) {
    auto a = p.begin(), b = root.begin();
    for (; b != root.end(); ++b, ++a)
        if (a == p.end() || *a != *b)
            return false;
    return true;
}
fs::path safePath(const fs::path &root, const std::string &r) {
    require(!root.empty() && !r.empty() && r.find('\0') == r.npos, "非法文件路径");
    fs::path rel = r;
    require(!rel.is_absolute(), "非法文件路径");
    auto p = normalized(root / rel);
    require(inside(p, normalized(root)), "非法文件路径");
    return p;
}
bool raw(const fs::path &p) {
    auto e = lower(p.extension());
    return e == ".fit" || e == ".fits";
}
bool photo(const fs::path &p) {
    auto e = lower(p.extension());
    return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".tif" || e == ".tiff";
}
bool media(const fs::path &p) {
    return raw(p) || photo(p);
}
J stamp(const fs::path &p) {
    struct stat s{};
    require(lstat(p.c_str(), &s) == 0, "无法读取文件：" + p.string());
    return J::array({s.st_dev, s.st_ino, s.st_size,
                     int64_t(s.st_mtimespec.tv_sec) * 1000000000 + s.st_mtimespec.tv_nsec,
                     int64_t(s.st_ctimespec.tv_sec) * 1000000000 + s.st_ctimespec.tv_nsec});
}
J rootStamp(const fs::path &p) {
    auto s = stamp(p);
    require(fs::is_directory(p) && !fs::is_symlink(p), "目录已失效");
    return J::array({s[0], s[1]});
}
J regularStamp(const fs::path &root, const fs::path &rel) {
    auto p = (root / rel).lexically_normal();
    require(!rel.is_absolute() && inside(p, root) && normalized(p) == p && fs::is_regular_file(p) &&
                !fs::is_symlink(p),
            "文件已移动、删除或变为链接");
    return stamp(p);
}
static bool isSubDirectory(const fs::path &path) {
    auto name = lower(path.filename().string());
    return name.size() >= 4 && name.substr(name.size() - 4) == "_sub";
}
std::vector<fs::path> mediaFiles(const fs::path &root, const std::function<bool()> &cancel, bool includeSub) {
    std::vector<fs::path> out;
    require(fs::is_directory(root), "目录不存在：" + root.string());
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (cancel && cancel())
            break;
        if (it->is_symlink()) {
            it.disable_recursion_pending();
            continue;
        }
        if (!includeSub && it->is_directory() && isSubDirectory(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() && media(it->path()) && inside(normalized(it->path()), root))
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}
std::string canonical(std::string n) {
    n = trim(n);
    return trim(n.substr(0, n.find(" - ")));
}
std::pair<std::string, bool> mediaTarget(const fs::path &rel, bool merge) {
    std::string category = "未分类";
    bool sub = false;
    if (rel.has_parent_path())
        category = rel.begin()->string();
    for (const auto &p : rel.parent_path()) {
        auto s = p.string();
        if (isSubDirectory(p)) {
            category = s.substr(0, s.size() - 4);
            sub = true;
            break;
        }
    }
    return {merge ? canonical(category) : category, sub};
}
Bytes readBytes(const fs::path &p, size_t limit) {
    std::ifstream in(p, std::ios::binary);
    require(bool(in), "无法读取：" + p.string());
    std::error_code ec;
    auto size = fs::file_size(p, ec);
    require(!ec && size <= limit, "文件过大或不可读：" + p.string());
    Bytes b(size, 0);
    in.read(b.data(), b.size());
    require(size_t(in.gcount()) == size, "文件读取不完整");
    return b;
}
J readJSON(const fs::path &p, J fallback) {
    if (!fs::exists(p))
        return fallback;
    auto j = J::parse(readBytes(p, 64 * 1024 * 1024));
    require(j.is_object(), "JSON 文件格式错误：" + p.string());
    return j;
}
void atomicWrite(const fs::path &p, const Bytes &b) {
    bool existed = fs::exists(p.parent_path());
    fs::create_directories(p.parent_path());
    if (!existed)
        chmod(p.parent_path().c_str(), 0700);
    std::string pattern = (p.parent_path() / ".astrolibrary-write-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    int fd = mkstemp(name.data());
    require(fd >= 0, "无法创建临时文件");
    try {
        fchmod(fd, 0600);
        size_t pos = 0;
        while (pos < b.size()) {
            ssize_t n = write(fd, b.data() + pos, b.size() - pos);
            if (n < 0 && errno == EINTR)
                continue;
            require(n > 0, "文件写入失败");
            pos += n;
        }
        require(fsync(fd) == 0, "文件同步失败");
        close(fd);
        fd = -1;
        require(rename(name.data(), p.c_str()) == 0, "文件保存失败");
        int dir = open(p.parent_path().c_str(), O_RDONLY);
        if (dir >= 0) {
            fsync(dir);
            close(dir);
        }
    } catch (...) {
        if (fd >= 0)
            close(fd);
        unlink(name.data());
        throw;
    }
}
void writeJSON(const fs::path &p, const J &j) {
    atomicWrite(p, j.dump(2) + "\n");
}
void validateRoots(const fs::path &s, const fs::path &d) {
    require(fs::is_directory(s), "源目录不存在");
    require(!inside(s, d) && !inside(d, s), "源目录和目标目录不能相同或互相包含");
}
bool copyFile(const fs::path &s, const fs::path &d, bool overwrite) {
    auto before = stamp(s);
    require(normalized(d.parent_path()) == d.parent_path() && !fs::is_symlink(d), "目标路径包含链接");
    fs::create_directories(d.parent_path());
    require(normalized(d.parent_path()) == d.parent_path() && !fs::is_symlink(d), "目标路径包含链接");
    if (fs::exists(d) && !overwrite)
        return false;
    auto tmp = d.parent_path() / (".astrolibrary-import-" + randomToken() + ".tmp");
    try {
        fs::copy_file(s, tmp);
        fs::last_write_time(tmp, fs::last_write_time(s));
        require(stamp(s) == before, "源文件正在变化");
        if (overwrite) {
            fs::rename(tmp, d);
        } else {
            if (link(tmp.c_str(), d.c_str()) != 0) {
                if (errno == EEXIST) {
                    fs::remove(tmp);
                    return false;
                }
                require(renameatx_np(AT_FDCWD, tmp.c_str(), AT_FDCWD, d.c_str(), RENAME_EXCL) == 0,
                        "目标文件写入失败");
            } else
                fs::remove(tmp);
        }
        return true;
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}
bool equalFiles(const fs::path &a, const fs::path &b) {
    if (fs::file_size(a) != fs::file_size(b))
        return false;
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    require(bool(x) && bool(y), "无法校验照片");
    char bx[262144], by[262144];
    while (x) {
        x.read(bx, sizeof(bx));
        y.read(by, sizeof(by));
        if (x.gcount() != y.gcount() || memcmp(bx, by, x.gcount()))
            return false;
    }
    return !x.bad() && !y.bad();
}
std::string urlEncode(const std::string &s) {
    std::string out;
    const char *h = "0123456789ABCDEF";
    for (unsigned char c : s)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~' || c == '/')
            out += c;
        else {
            out += '%';
            out += h[c >> 4];
            out += h[c & 15];
        }
    return out;
}
std::string urlDecode(const std::string &s) {
    std::string out;
    auto digit = [](char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && digit(s[i + 1]) >= 0 && digit(s[i + 2]) >= 0) {
            out += char(digit(s[i + 1]) * 16 + digit(s[i + 2]));
            i += 2;
        } else
            out += s[i];
    }
    return out;
}
J sourceStatus(const fs::path &p) {
    J j = {{"path", p.string()},
           {"exists", false},
           {"isDirectory", false},
           {"accessible", false},
           {"permissionRequired", false},
           {"hasEntries", false},
           {"mediaCount", 0},
           {"reason", p.empty() ? "未配置" : "目录不存在"}};
    if (p.empty())
        return j;
    try {
        j["exists"] = fs::exists(p);
        j["isDirectory"] = fs::is_directory(p);
        if (j["isDirectory"] == true) {
            fs::directory_iterator it(p);
            j["hasEntries"] = (it != fs::directory_iterator());
            j["accessible"] = true;
            j["reason"] = "";
        }
    } catch (const fs::filesystem_error &e) {
        j["reason"] = e.what();
        j["permissionRequired"] = (e.code() == std::errc::permission_denied);
    }
    return j;
}
J detectSeestar() {
    J out = J::array();
    auto root = home() / "Library/Containers/com.zwoseestar.iscope";
    if (!fs::is_directory(root))
        return out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_symlink() || it.depth() > 7) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory() && lower(it->path().filename()) == "myworks") {
            auto j = sourceStatus(it->path());
            j["score"] = j["accessible"] == true ? 100 : 0;
            j["label"] = it->path().string();
            out.push_back(j);
            it.disable_recursion_pending();
        }
    }
    return out;
}
} // namespace astro
