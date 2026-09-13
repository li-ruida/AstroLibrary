#include "core.hpp"
#include <regex>
namespace astro {
J r2Config(J p, const std::string &existing) {
    require(p.is_object(), "R2 配置必须是对象");
    J c = {{"enabled", false},    {"autoUpload", false},
           {"accountId", ""},     {"bucket", ""},
           {"accessKeyId", ""},   {"secretAccessKey", ""},
           {"publicBaseUrl", ""}, {"keyPrefix", "astrolibrary/previews"}};
    for (auto it = c.begin(); it != c.end(); ++it)
        if (p.contains(it.key())) {
            require(p[it.key()].type() == it.value().type(), "R2 配置类型错误");
            it.value() = p[it.key()];
        }
    for (auto key : {"accountId", "bucket", "accessKeyId", "publicBaseUrl", "keyPrefix"})
        c[key] = trim(c[key]);
    c["accountId"] = lower(c["accountId"]);
    c["bucket"] = lower(c["bucket"]);
    if (c["secretAccessKey"] == "")
        c["secretAccessKey"] = existing;
    auto prefix = c["keyPrefix"].get<std::string>();
    if (prefix.empty())
        prefix = "astrolibrary/previews";
    while (!prefix.empty() && prefix.front() == '/')
        prefix.erase(0, 1);
    while (!prefix.empty() && prefix.back() == '/')
        prefix.pop_back();
    require(!prefix.empty() && prefix.size() <= 512, "R2 前缀无效");
    std::string cleaned;
    std::istringstream parts(prefix);
    std::string part;
    while (std::getline(parts, part, '/')) {
        part = trim(part);
        require(!part.empty() && part != "." && part != ".." && part.size() <= 128 &&
                    std::none_of(part.begin(), part.end(), [](unsigned char ch) { return ch < 32; }),
                "R2 对象前缀包含非法路径");
        if (!cleaned.empty())
            cleaned += '/';
        cleaned += part;
    }
    c["keyPrefix"] = cleaned;
    auto url = c["publicBaseUrl"].get<std::string>();
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    require(url.empty() || std::regex_match(url, std::regex("https://[A-Za-z0-9.-]+(:443)?")),
            "公开访问地址必须是仅含域名的 HTTPS 地址");
    c["publicBaseUrl"] = url;
    bool any = false;
    for (auto k : {"accountId", "bucket", "accessKeyId", "secretAccessKey"})
        any |= c[k] != "";
    if (any || c["enabled"] == true || c["autoUpload"] == true) {
        require(std::regex_match(c["accountId"].get<std::string>(), std::regex("[0-9a-f]{32}")),
                "Cloudflare Account ID 必须是 32 位十六进制字符");
        auto bucket = c["bucket"].get<std::string>();
        require(std::regex_match(bucket, std::regex("[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]")) &&
                    bucket.find("..") == bucket.npos && bucket.find(".-") == bucket.npos &&
                    bucket.find("-.") == bucket.npos,
                "R2 存储桶名称格式不正确");
        require(std::regex_match(c["accessKeyId"].get<std::string>(), std::regex("[A-Za-z0-9_-]{16,128}")),
                "R2 Access Key ID 格式不正确");
        auto secret = c["secretAccessKey"].get<std::string>();
        require(!secret.empty() && secret.size() <= 512 &&
                    std::all_of(secret.begin(), secret.end(),
                                [](unsigned char ch) { return ch >= 33 && ch <= 126; }),
                "R2 Secret Access Key 格式不正确");
    }
    require(c["autoUpload"] == false || c["enabled"] == true, "开启自动上传前请先启用 R2");
    return c;
}
J r2Public(const J &c) {
    J p = c;
    p.erase("secretAccessKey");
    p["secretConfigured"] = c.at("secretAccessKey") != "";
    p["configured"] = c.at("accountId") != "" && c.at("bucket") != "" && c.at("accessKeyId") != "" &&
                      c.at("secretAccessKey") != "";
    p["endpoint"] = c.at("accountId") == ""
                        ? ""
                        : "https://" + c.at("accountId").get<std::string>() + ".r2.cloudflarestorage.com";
    return p;
}
J sigv4(const std::string &method, const std::string &host, const std::string &uri, const Bytes &body,
        const std::string &key, const std::string &secret, const std::string &date) {
    std::string amz = date.empty() ? utcTime("%Y%m%dT%H%M%SZ") : date, day = amz.substr(0, 8),
                hash = sha256(body);
    J h = {{"host", host},
           {"x-amz-content-sha256", hash},
           {"x-amz-date", amz},
           {"content-type", method == "PUT" ? "image/jpeg" : "application/octet-stream"}};
    if (method == "PUT")
        h["cache-control"] = "public, max-age=31536000, immutable";
    std::string names, canonical;
    for (auto it = h.begin(); it != h.end(); ++it) {
        if (!names.empty())
            names += ';';
        names += it.key();
        canonical += it.key() + ":" + it.value().get<std::string>() + "\n";
    }
    auto scope = day + "/auto/s3/aws4_request";
    auto request = method + "\n" + uri + "\n\n" + canonical + "\n" + names + "\n" + hash;
    auto signing = hmac(hmac(hmac(hmac("AWS4" + secret, day), "auto"), "s3"), "aws4_request");
    auto signature = hex(hmac(signing, "AWS4-HMAC-SHA256\n" + amz + "\n" + scope + "\n" + sha256(request)));
    h.erase("host");
    h["Authorization"] = "AWS4-HMAC-SHA256 Credential=" + key + "/" + scope + ", SignedHeaders=" + names +
                         ", Signature=" + signature;
    return h;
}
J r2Request(const J &c, const std::string &method, const std::string &key, const Bytes &body) {
    require(r2Public(c)["configured"] == true, "R2 配置不完整");
    auto host = c.at("accountId").get<std::string>() + ".r2.cloudflarestorage.com";
    auto path = urlEncode("/" + c.at("bucket").get<std::string>() + (key.empty() ? "" : "/" + key));
    auto r =
        platformHTTPS(method, "https://" + host + path,
                      sigv4(method, host, path, body, c.at("accessKeyId"), c.at("secretAccessKey")), body);
    int status = r["status"];
    require(status >= 200 && status < 300,
            "R2 返回 HTTP " + std::to_string(status) + "：" + r.value("body", std::string()).substr(0, 300));
    return r;
}
} // namespace astro
