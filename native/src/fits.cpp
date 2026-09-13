#include "core.hpp"
#include <cstring>
#include <fcntl.h>
#include <list>
#include <regex>
#include <sqlite3.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
namespace astro {
static std::string cardValue(const std::string &card) {
    auto s = trim(card.substr(10));
    if (s.empty() || s[0] != '\'')
        return trim(s.substr(0, s.find('/')));
    std::string out;
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i] == '\'') {
            if (i + 1 >= s.size() || s[i + 1] != '\'')
                break;
            ++i;
        }
        out += s[i];
    }
    return trim(out);
}
static bool readCards(const char *data, size_t len, J &h, bool history) {
    for (size_t i = 0; i + 80 <= len; i += 80) {
        std::string card(data + i, 80);
        for (char &c : card)
            if ((unsigned char)c > 127)
                c = '?';
        auto k = trim(card.substr(0, 8));
        if (k == "END")
            return true;
        if (history && k == "HISTORY")
            h["_HISTORY"] = h.value("_HISTORY", std::string()) + trim(card.substr(8)) + "\n";
        if (card.substr(8, 2) == "= ")
            h[k] = cardValue(card);
    }
    return false;
}
J fitsHeader(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return J::object();
    char b[2880 * 8];
    f.read(b, sizeof(b));
    J h = J::object();
    readCards(b, f.gcount(), h, false);
    return h;
}
static double number(const J &h, const char *k, const char *fallback) {
    std::string s = h.value(k, std::string(fallback));
    std::replace(s.begin(), s.end(), 'D', 'E');
    size_t end = 0;
    double v = std::stod(s, &end);
    require(end == s.size() && std::isfinite(v), "FITS 数值无效");
    return v;
}
static int64_t integer(const J &h, const std::string &k, const char *fallback = "0") {
    auto s = h.value(k, std::string(fallback));
    size_t end;
    int64_t v = std::stoll(s, &end);
    require(end == s.size(), "FITS 整数无效");
    return v;
}
struct Mapping {
    int fd = -1;
    const unsigned char *data = nullptr;
    size_t size = 0;
    explicit Mapping(const fs::path &p) {
        fd = open(p.c_str(), O_RDONLY | O_NOFOLLOW);
        require(fd >= 0, "无法打开 FITS");
        struct stat st{};
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            close(fd);
            throw std::runtime_error("FITS 文件无效");
        }
        size = st.st_size;
        void *m = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("无法映射 FITS");
        }
        data = (const unsigned char *)m;
        madvise((void *)data, size, MADV_SEQUENTIAL);
    }
    ~Mapping() {
        if (data)
            munmap((void *)data, size);
        if (fd >= 0)
            close(fd);
    }
};
Sample sampleFits(const fs::path &path) {
    Mapping m(path);
    J h;
    uint64_t offset = 0, w = 0, height = 0, planes = 0;
    int bitpix = 0;
    bool found = false;
    auto product = [&](uint64_t a, uint64_t b) {
        require(b == 0 || a <= uint64_t(m.size) / b, "FITS 维度超过文件大小");
        return a * b;
    };
    for (int hdu = 0; hdu < 64 && !found; hdu++) {
        h = J::object();
        bool ended = false;
        for (int block = 0; block < 256; block++) {
            require(offset <= m.size && m.size - offset >= 2880, "FITS 头不完整");
            if (block == 0)
                require(memcmp(m.data + offset, hdu ? "XTENSION" : "SIMPLE  ", 8) == 0,
                        "不是有效的 FITS 文件");
            ended = readCards((const char *)m.data + offset, 2880, h, true);
            offset += 2880;
            if (ended)
                break;
        }
        require(ended, "FITS 头过长或缺少 END");
        bitpix = (int)integer(h, "BITPIX");
        auto naxis = integer(h, "NAXIS");
        require(
            (bitpix == 8 || bitpix == 16 || bitpix == 32 || bitpix == 64 || bitpix == -32 || bitpix == -64) &&
                naxis >= 0 && naxis <= 9,
            "不支持的 FITS 像素类型或维度");
        uint64_t count = naxis ? 1 : 0;
        std::vector<uint64_t> axes;
        for (int i = 1; i <= naxis; i++) {
            auto n = integer(h, "NAXIS" + std::to_string(i));
            require(n >= 0, "FITS 维度无效");
            axes.push_back(n);
            count = product(count, n);
        }
        auto pc = integer(h, "PCOUNT"), gc = integer(h, "GCOUNT", "1");
        require(pc >= 0 && gc >= 1 && uint64_t(pc) <= m.size - count, "FITS 维度无效");
        auto size = product(product(count + pc, gc), abs(bitpix) / 8);
        require(offset <= m.size && size <= m.size - offset, "FITS 像素数据不完整");
        if ((hdu == 0 || h.value("XTENSION", "") == "IMAGE") && count) {
            require(h.value("GROUPS", "") != "T" && pc == 0 && gc == 1, "暂不支持 FITS 随机组");
            require(naxis == 2 || (naxis == 3 && (axes[2] == 1 || axes[2] == 3)),
                    "暂不支持此 FITS 数据立方体");
            w = axes[0];
            height = axes[1];
            planes = naxis == 3 ? axes[2] : 1;
            found = true;
            break;
        }
        offset += ((size + 2879) / 2880) * 2880;
    }
    require(found, "没有可预览的 FITS 图像 HDU");
    double scale = number(h, "BSCALE", "1"), zero = number(h, "BZERO", "0");
    std::string pattern = h.value("BAYERPAT", "");
    pattern.erase(std::remove(pattern.begin(), pattern.end(), ' '), pattern.end());
    for (char &c : pattern)
        c = toupper(c);
    bool bayer =
        planes == 1 && (pattern == "RGGB" || pattern == "BGGR" || pattern == "GRBG" || pattern == "GBRG");
    require(!bayer || (w >= 2 && height >= 2), "Bayer 图像尺寸不足");
    uint64_t lw = bayer ? w / 2 : w, lh = bayer ? height / 2 : height;
    double ratio = std::min(1.0, 960.0 / std::max(lw, lh));
    Sample s;
    s.w = std::max(1, (int)(lw * ratio));
    s.h = std::max(1, (int)(lh * ratio));
    s.c = bayer || planes == 3 ? 3 : 1;
    s.bitpix = bitpix;
    s.header = h;
    s.values.resize(size_t(s.w) * s.h * s.c);
    bool hasBlank = bitpix > 0 && h.contains("BLANK");
    int64_t blank = hasBlank ? integer(h, "BLANK") : 0;
    auto pixel = [&](uint64_t x, uint64_t y, int plane) -> double {
        auto p = m.data + offset + ((uint64_t(plane) * height + y) * w + x) * (abs(bitpix) / 8);
        uint64_t bits = 0;
        for (int b = 0; b < abs(bitpix) / 8; b++)
            bits = (bits << 8) | p[b];
        double v = 0;
        if (bitpix > 0) {
            int64_t n = bitpix == 8    ? int64_t(bits)
                        : bitpix == 16 ? int16_t(bits)
                        : bitpix == 32 ? int32_t(bits)
                                       : int64_t(bits);
            if (hasBlank && n == blank)
                return NAN;
            v = (double)n;
        } else if (bitpix == -32) {
            uint32_t b = bits;
            float f;
            memcpy(&f, &b, 4);
            v = f;
        } else
            memcpy(&v, &bits, 8);
        return v * scale + zero;
    };
    int xo = ((integer(h, "XBAYROFF") % 2) + 2) % 2, yo = ((integer(h, "YBAYROFF") % 2) + 2) % 2;
    std::vector<std::pair<int, int>> positions[3];
    if (bayer)
        for (int c = 0; c < 3; c++)
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 2; x++)
                    if (pattern[((y + yo) % 2) * 2 + (x + xo) % 2] == "RGB"[c])
                        positions[c].push_back({x, y});
    for (int plane = 0; plane < (bayer ? 1 : s.c); plane++)
        for (int row = s.h - 1; row >= 0; row--) {
            uint64_t y = std::min(lh - 1, (uint64_t)((s.h - row - .5) * lh / s.h));
            for (int col = 0; col < s.w; col++) {
                uint64_t x = std::min(lw - 1, (uint64_t)((col + .5) * lw / s.w));
                size_t i = (size_t(row) * s.w + col) * s.c;
                if (bayer) {
                    for (int c = 0; c < 3; c++) {
                        double sum = 0;
                        int n = 0;
                        for (auto [dx, dy] : positions[c]) {
                            double v = pixel(x * 2 + dx, y * 2 + dy, 0);
                            if (std::isfinite(v)) {
                                sum += v;
                                n++;
                            }
                        }
                        s.values[i + c] = n ? sum / n : NAN;
                    }
                } else
                    s.values[i + plane] = pixel(x, y, plane);
            }
        }
    return s;
}
void Options::validate() {
    require(mode == "auto" || mode == "stretch" || mode == "linear", "未知 FIT 预览模式");
    require(std::isfinite(black) && black >= -3 && black <= 3, "黑场调节必须介于 -3 和 3");
    require(std::isfinite(brightness) && brightness >= -2 && brightness <= 2, "亮度调节必须介于 -2 和 2");
    black = std::nearbyint(black * 100) / 100;
    brightness = std::nearbyint(brightness * 100) / 100;
}
static double median(std::vector<double> v) {
    require(!v.empty(), "FITS 没有有效像素");
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : v[n / 2 - 1] + (v[n / 2] - v[n / 2 - 1]) * .5;
}
static void be32(Bytes &b, uint32_t v) {
    for (int i = 24; i >= 0; i -= 8)
        b += char(v >> i);
}
static uint32_t get32(const char *p) {
    uint32_t n = 0;
    for (int i = 0; i < 4; i++)
        n = (n << 8) | (unsigned char)p[i];
    return n;
}
static Bytes compress(const Bytes &b, int level) {
    uLongf n = compressBound(b.size());
    Bytes out(n, 0);
    require(compress2((Bytef *)out.data(), &n, (const Bytef *)b.data(), b.size(), level) == Z_OK, "压缩失败");
    out.resize(n);
    return out;
}
static void chunk(Bytes &out, const char *name, const Bytes &b) {
    be32(out, b.size());
    Bytes block = std::string(name, 4) + b;
    out += block;
    be32(out, (uint32_t)crc32(0, (const Bytef *)block.data(), block.size()));
}
Bytes renderSample(const Sample &s, const Options &input) {
    Options o = input;
    o.validate();
    auto &v = s.values;
    size_t stride = std::max<size_t>(1, v.size() / s.c / 16000) * s.c;
    std::vector<double> sampled, levels, finite;
    for (size_t i = 0; i < v.size(); i += stride) {
        bool good = true;
        for (int c = 0; c < s.c; c++)
            good &= std::isfinite(v[i + c]);
        if (good)
            for (int c = 0; c < s.c; c++)
                sampled.push_back(v[i + c]);
    }
    require(!sampled.empty(), "FITS 没有有效像素");
    auto [mi, ma] = std::minmax_element(sampled.begin(), sampled.end());
    double low = 0, high = 0;
    if (s.bitpix > 0) {
        double scale = number(s.header, "BSCALE", "1"), zero = number(s.header, "BZERO", "0");
        low = (s.bitpix == 8 ? 0 : -std::pow(2, s.bitpix - 1)) * scale + zero;
        high = (s.bitpix == 8 ? 255 : std::pow(2, s.bitpix - 1) - 1) * scale + zero;
        if (low > high)
            std::swap(low, high);
        if (*mi >= 0)
            low = std::max(0.0, low);
    } else if (*mi >= -.1 && *ma <= 1.1) {
        low = 0;
        high = 1;
    } else if (*mi >= 0 && *ma > 255 && *ma <= 65535) {
        low = 0;
        high = 65535;
    } else {
        low = std::min(0.0, *mi);
        high = *ma;
    }
    double span = high - low;
    if (!span)
        span = 1;
    for (auto &x : sampled)
        x = (x - low) / span;
    for (size_t i = 0; i < sampled.size(); i += s.c) {
        double sum = 0;
        for (int c = 0; c < s.c; c++)
            sum += sampled[i + c];
        levels.push_back(sum / s.c);
    }
    double med = median(levels);
    std::vector<double> deviations;
    for (double x : levels)
        deviations.push_back(abs(x - med));
    double sigma = 1.4826 * median(deviations);
    static std::regex processed(
        "\\b(autostretch|asinh|generalized hyperbolic|histogram transformation|midtone stretch)\\b",
        std::regex::icase);
    bool stretch =
        o.mode == "stretch" || (o.mode == "auto" && med < .08 &&
                                !std::regex_search(s.header.value("_HISTORY", std::string()), processed));
    double offsets[3] = {};
    if (o.neutralize && s.c == 3 && sigma > 0) {
        for (int c = 0; c < 3; c++) {
            std::vector<double> b;
            for (size_t i = 0; i < levels.size(); i++)
                if (levels[i] <= med)
                    b.push_back(sampled[i * 3 + c]);
            offsets[c] = median(b);
        }
        double center = (offsets[0] + offsets[1] + offsets[2]) / 3;
        for (double &x : offsets)
            x -= center;
    }
    double shadow = stretch ? std::max(0.0, med - 2.8 * sigma) : 0;
    shadow = std::clamp(shadow + o.black * std::max({sigma, abs(med) * .01, 1e-30}), -.5, .99);
    double x = std::clamp((med - shadow) / (1 - shadow), 1e-300, .999999), target = .1,
           midtone = x * (target - 1) / (2 * target * x - target - x), gain = std::pow(2, o.brightness);
    Bytes pixels;
    pixels.reserve(v.size() + s.h);
    for (int y = 0; y < s.h; y++) {
        pixels += char(0);
        for (int col = 0; col < s.w * s.c; col++) {
            size_t i = size_t(y) * s.w * s.c + col;
            double level = 0;
            if (std::isfinite(v[i])) {
                level =
                    std::clamp(((v[i] - low) / span - offsets[i % s.c] - shadow) / (1 - shadow), 0.0, 1.0);
                if (stretch && level > 0 && level < 1)
                    level = (midtone - 1) * level / ((2 * midtone - 1) * level - midtone);
                level = gain * level / (1 + (gain - 1) * level);
            }
            pixels += char((unsigned char)std::nearbyint(255 * level));
        }
    }
    Bytes png("\x89PNG\r\n\x1a\n", 8), hdr;
    be32(hdr, s.w);
    be32(hdr, s.h);
    hdr += char(8);
    hdr += char(s.c == 3 ? 2 : 0);
    hdr.append(3, 0);
    chunk(png, "IHDR", hdr);
    chunk(png, "IDAT", compress(pixels, 3));
    chunk(png, "IEND", "");
    return png;
}
static Bytes encodeSample(const Sample &s) {
    Bytes meta = J::array({s.w, s.h, s.c, s.header, s.bitpix}).dump();
    Bytes out;
    be32(out, meta.size());
    out += meta;
    out += compress(Bytes((const char *)s.values.data(), s.values.size() * 8), 1);
    return out;
}
static Sample decodeSample(const Bytes &b) {
    require(b.size() >= 4, "缓存无效");
    size_t n = get32(b.data());
    require(n <= 1000000 && n + 4 < b.size(), "缓存无效");
    auto j = J::parse(b.substr(4, n));
    Sample s;
    s.w = j.at(0);
    s.h = j.at(1);
    s.c = j.at(2);
    s.header = j.at(3);
    s.bitpix = j.at(4);
    require(s.w > 0 && s.w <= 960 && s.h > 0 && s.h <= 960 && (s.c == 1 || s.c == 3) && s.header.is_object(),
            "缓存无效");
    s.values.resize(size_t(s.w) * s.h * s.c);
    uLongf bytes = s.values.size() * 8;
    require(uncompress((Bytef *)s.values.data(), &bytes, (const Bytef *)b.data() + n + 4, b.size() - n - 4) ==
                    Z_OK &&
                bytes == s.values.size() * 8,
            "缓存损坏");
    return s;
}
struct FitsCache::Impl {
    fs::path root;
    sqlite3 *db = nullptr;
    std::mutex mutex;
    std::mutex statusMutex;
    J status;
    std::string generation = randomToken();
    double retry = 0;
    std::list<std::pair<std::string, Bytes>> previews;
    std::list<std::pair<std::string, Sample>> samples;
    std::map<std::string, J> headers;
    size_t memory = 0;
    int dataVersion = -1;
    bool totalsValid = false;
    int64_t diskEntries = 0, diskBytes = 0;
    explicit Impl(fs::path p) : root(p) {
        status = {{"entries", 0},          {"bytes", 0},
                  {"memoryBytes", 0},      {"limitBytes", int64_t(2) * 1024 * 1024 * 1024},
                  {"path", root.string()}, {"error", ""}};
    }
    ~Impl() {
        if (db)
            sqlite3_close(db);
    }
    void sql(const char *s) {
        char *err = nullptr;
        int rc = sqlite3_exec(db, s, nullptr, nullptr, &err);
        std::string message = err ? err : "SQLite 错误";
        sqlite3_free(err);
        require(rc == SQLITE_OK, message);
    }
    void fail(const std::exception &e) {
        std::lock_guard l(statusMutex);
        status["error"] = e.what();
        retry = now() + 60;
        totalsValid = false;
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
    }
    bool open() {
        if (db)
            return true;
        if (now() < retry)
            return false;
        try {
            require(!fs::is_symlink(root), "缓存目录不能为符号链接");
            require(fs::is_directory(root.parent_path()), "缓存目录的父目录不存在");
            fs::create_directories(root);
            chmod(root.c_str(), 0700);
            auto path = root / "fits-v1.sqlite3";
            for (auto suffix : {"", "-wal", "-shm", "-journal"})
                require(!fs::is_symlink(path.string() + suffix), "缓存数据库不能为符号链接");
            require(sqlite3_open_v2(path.c_str(), &db,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                    nullptr) == SQLITE_OK,
                    "无法打开缓存数据库");
            sqlite3_busy_timeout(db, 2000);
            chmod(path.c_str(), 0600);
            sql("PRAGMA auto_vacuum=INCREMENTAL; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA "
                "wal_autocheckpoint=64; CREATE TABLE IF NOT EXISTS entries(key TEXT PRIMARY KEY,kind TEXT "
                "NOT NULL,value BLOB NOT NULL,checksum TEXT NOT NULL,size INTEGER NOT NULL,touched REAL NOT "
                "NULL); CREATE INDEX IF NOT EXISTS cache_lru ON entries(touched);");
            std::lock_guard l(statusMutex);
            status["error"] = "";
            totalsValid = false;
            return true;
        } catch (const std::exception &e) {
            fail(e);
            return false;
        }
    }
    Bytes get(const std::string &key, const char *kind) {
        if (!open())
            return "";
        sqlite3_stmt *q = nullptr;
        Bytes b;
        if (sqlite3_prepare_v2(db, "SELECT value,checksum FROM entries WHERE key=? AND kind=?", -1, &q,
                               nullptr) == SQLITE_OK) {
            sqlite3_bind_text(q, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 2, kind, -1, SQLITE_STATIC);
            if (sqlite3_step(q) == SQLITE_ROW) {
                int n = sqlite3_column_bytes(q, 0);
                if (n > 0 && n < 32 * 1024 * 1024) {
                    b.assign((const char *)sqlite3_column_blob(q, 0), n);
                    auto sum = (const char *)sqlite3_column_text(q, 1);
                    if (!sum || sha256(b) != sum)
                        b.clear();
                }
            }
        }
        sqlite3_finalize(q);
        if (!b.empty()) {
            sqlite3_prepare_v2(db, "UPDATE entries SET touched=? WHERE key=? AND touched<?", -1, &q, nullptr);
            sqlite3_bind_double(q, 1, now());
            sqlite3_bind_text(q, 2, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(q, 3, now() - 30);
            sqlite3_step(q);
            sqlite3_finalize(q);
        }
        return b;
    }
    void refreshTotals() {
        sqlite3_stmt *q = nullptr;
        int version = 0;
        int rc = sqlite3_prepare_v2(db, "PRAGMA data_version", -1, &q, nullptr);
        if (rc == SQLITE_OK)
            rc = sqlite3_step(q);
        if (rc == SQLITE_ROW)
            version = sqlite3_column_int(q, 0);
        sqlite3_finalize(q);
        require(rc == SQLITE_ROW, "无法读取缓存版本");
        if (totalsValid && version == dataVersion)
            return;
        q = nullptr;
        rc = sqlite3_prepare_v2(db, "SELECT count(*),coalesce(sum(size),0) FROM entries", -1, &q, nullptr);
        if (rc == SQLITE_OK)
            rc = sqlite3_step(q);
        if (rc == SQLITE_ROW) {
            diskEntries = sqlite3_column_int64(q, 0);
            diskBytes = sqlite3_column_int64(q, 1);
        }
        sqlite3_finalize(q);
        require(rc == SQLITE_ROW, "无法读取缓存占用");
        dataVersion = version;
        totalsValid = true;
    }
    void put(const std::string &key, const char *kind, const Bytes &b) {
        if (!open())
            return;
        sqlite3_stmt *q = nullptr;
        try {
            // Serialize accounting with other App/CLI writers. data_version
            // invalidates local totals when another SQLite connection commits.
            sql("BEGIN IMMEDIATE");
            refreshTotals();
            require(sqlite3_prepare_v2(db, "SELECT size FROM entries WHERE key=?", -1, &q, nullptr) ==
                        SQLITE_OK,
                    "无法读取旧缓存占用");
            sqlite3_bind_text(q, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            int rc = sqlite3_step(q);
            require(rc == SQLITE_ROW || rc == SQLITE_DONE, "无法读取旧缓存占用");
            bool replacing = rc == SQLITE_ROW;
            int64_t oldBytes = replacing ? sqlite3_column_int64(q, 0) : 0;
            sqlite3_finalize(q);
            q = nullptr;
            require(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO entries VALUES(?,?,?,?,?,?)", -1, &q,
                                       nullptr) == SQLITE_OK,
                    "缓存准备写入失败");
            auto sum = sha256(b);
            sqlite3_bind_text(q, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 2, kind, -1, SQLITE_STATIC);
            sqlite3_bind_blob(q, 3, b.data(), (int)b.size(), SQLITE_TRANSIENT);
            sqlite3_bind_text(q, 4, sum.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(q, 5, b.size());
            sqlite3_bind_double(q, 6, now());
            rc = sqlite3_step(q);
            sqlite3_finalize(q);
            q = nullptr;
            require(rc == SQLITE_DONE, "缓存写入失败");
            diskEntries += replacing ? 0 : 1;
            diskBytes += int64_t(b.size()) - oldBytes;
            std::vector<std::pair<std::string, int64_t>> victims;
            if (diskBytes > 2147483648LL) {
                require(sqlite3_prepare_v2(db, "SELECT key,size FROM entries ORDER BY touched ASC", -1, &q,
                                           nullptr) == SQLITE_OK,
                        "无法选择待淘汰缓存");
                int64_t remaining = diskBytes;
                while (remaining > 2147483648LL && (rc = sqlite3_step(q)) == SQLITE_ROW) {
                    auto victim = std::string((const char *)sqlite3_column_text(q, 0));
                    int64_t size = sqlite3_column_int64(q, 1);
                    victims.emplace_back(std::move(victim), size);
                    remaining -= size;
                }
                require(remaining <= 2147483648LL, "缓存容量淘汰失败");
                sqlite3_finalize(q);
                q = nullptr;
                require(sqlite3_prepare_v2(db, "DELETE FROM entries WHERE key=?", -1, &q, nullptr) ==
                            SQLITE_OK,
                        "无法淘汰缓存");
                for (const auto &[victim, size] : victims) {
                    sqlite3_reset(q);
                    sqlite3_bind_text(q, 1, victim.c_str(), -1, SQLITE_TRANSIENT);
                    require(sqlite3_step(q) == SQLITE_DONE, "缓存淘汰失败");
                    diskEntries--;
                    diskBytes -= size;
                }
                sqlite3_finalize(q);
                q = nullptr;
            }
            sql("COMMIT");
            if (!victims.empty())
                sql("PRAGMA incremental_vacuum(64)");
        } catch (const std::exception &e) {
            sqlite3_finalize(q);
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            fail(e);
        }
    }
    void updateInfo() {
        size_t bytes = 0;
        for (auto &x : previews)
            bytes += x.second.size();
        for (auto &x : samples)
            bytes += x.second.values.size() * 8;
        if (db) {
            try {
                refreshTotals();
            } catch (const std::exception &e) {
                fail(e);
            }
        }
        std::lock_guard l(statusMutex);
        status["memoryBytes"] = bytes;
        if (totalsValid) {
            status["entries"] = diskEntries;
            status["bytes"] = diskBytes;
        }
    }
};
FitsCache::FitsCache(fs::path root) : impl(new Impl(root)) {
    std::lock_guard l(impl->mutex);
    impl->open();
    impl->updateInfo();
}
FitsCache::~FitsCache() = default;
static std::string cacheKey(const char *kind, const J &identity) {
    return sha256(J::array({kind, identity}).dump(-1, ' ', true));
}
static J sourceKey(const fs::path &p) {
    auto a = stamp(p);
    J key = J::array({normalized(p).string()});
    for (auto &v : a)
        key.push_back(v);
    return key;
}
J FitsCache::header(const fs::path &p) {
    std::lock_guard l(impl->mutex);
    auto sk = sourceKey(p);
    auto key = cacheKey("header", J::array({1, sk}));
    auto i = impl->headers.find(key);
    if (i != impl->headers.end())
        return i->second;
    J h;
    auto b = impl->get(key, "header");
    try {
        if (!b.empty())
            h = J::parse(b);
    } catch (...) {
    }
    if (!h.is_object()) {
        h = fitsHeader(p);
        if (!h.empty() && sourceKey(p) == sk)
            impl->put(key, "header", h.dump());
    }
    if (!h.empty()) {
        if (impl->headers.size() >= 4096)
            impl->headers.erase(impl->headers.begin());
        impl->headers[key] = h;
    }
    return h;
}
Bytes FitsCache::preview(const fs::path &p, Options o) {
    o.validate();
    std::lock_guard l(impl->mutex);
    auto sk = sourceKey(p);
    auto pk = cacheKey("png", J::array({1, 2, sk, o.json()}));
    for (auto it = impl->previews.begin(); it != impl->previews.end(); ++it)
        if (it->first == pk) {
            impl->previews.splice(impl->previews.begin(), impl->previews, it);
            return impl->previews.front().second;
        }
    auto b = impl->get(pk, "png");
    if (b.size() < 8 || b.substr(0, 8) != Bytes("\x89PNG\r\n\x1a\n", 8)) {
        auto key = cacheKey("sample", J::array({1, sk}));
        auto it =
            std::find_if(impl->samples.begin(), impl->samples.end(), [&](auto &s) { return s.first == key; });
        if (it == impl->samples.end()) {
            Sample s;
            try {
                s = decodeSample(impl->get(key, "sample"));
            } catch (...) {
                s = sampleFits(p);
                require(sourceKey(p) == sk, "FIT 文件正在变化，请稍后重试");
                impl->put(key, "sample", encodeSample(s));
            }
            impl->samples.push_front({key, std::move(s)});
        } else
            impl->samples.splice(impl->samples.begin(), impl->samples, it);
        b = renderSample(impl->samples.front().second, o);
        impl->put(pk, "png", b);
    }
    impl->previews.push_front({pk, b});
    size_t total = 0;
    for (auto it = impl->previews.begin(); it != impl->previews.end();) {
        total += it->second.size();
        if (total > 32 * 1024 * 1024 || std::distance(impl->previews.begin(), it) >= 128)
            it = impl->previews.erase(it);
        else
            ++it;
    }
    total = 0;
    for (auto it = impl->samples.begin(); it != impl->samples.end();) {
        total += it->second.values.size() * 8;
        if (total > 48 * 1024 * 1024 || std::distance(impl->samples.begin(), it) >= 8)
            it = impl->samples.erase(it);
        else
            ++it;
    }
    impl->updateInfo();
    return b;
}
J FitsCache::info() {
    if (impl->mutex.try_lock()) {
        impl->updateInfo();
        impl->mutex.unlock();
    }
    std::lock_guard l(impl->statusMutex);
    return impl->status;
}
void FitsCache::clear() {
    std::lock_guard l(impl->mutex);
    require(impl->open(), "缓存目录不可用");
    impl->totalsValid = false;
    impl->sql("DELETE FROM entries; PRAGMA wal_checkpoint(TRUNCATE); PRAGMA incremental_vacuum;");
    impl->previews.clear();
    impl->samples.clear();
    impl->headers.clear();
    impl->generation = randomToken();
    impl->updateInfo();
}
std::string FitsCache::etag(const fs::path &p, Options o) {
    o.validate();
    std::string gen;
    {
        std::lock_guard l(impl->mutex);
        gen = impl->generation;
    }
    return "\"" + cacheKey("etag", J::array({gen, 1, 2, sourceKey(p), o.json()})) + "\"";
}
} // namespace astro
