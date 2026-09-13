# 配置与安全

## 本机访问边界

服务只允许绑定回环地址：

```text
127.0.0.1
```

因此自定义端口只改变本机网页地址，不会让照片直接暴露到局域网。项目当前不提供远程访问或公网部署能力。

`--host` 只接受 `127.0.0.1`、`localhost` 或其他回环地址；传入 `0.0.0.0` 或局域网地址时服务会拒绝启动。需要跨设备访问时，请使用带认证和 HTTPS 的 SSH 隧道或反向代理，不要直接暴露 AstroLibrary。

服务会校验 HTTP `Host`，拒绝非回环来源；所有写请求还需要匹配本机 `Origin` 和进程级 CSRF Token，以阻止恶意网页通过 DNS 重绑定或跨站请求操作本机图库。

## 账号保护

网页账号保护默认关闭。开启后：

- 密码使用 PBKDF2-HMAC-SHA256、600,000 次迭代和随机 Salt 哈希保存
- 设置文件不保存明文密码，并尝试使用 `0600` 权限
- 登录后使用 HttpOnly、SameSite Cookie 会话
- 会话默认有效期 12 小时
- 连续登录失败会触发短暂限制
- 图库 API、源媒体和修图媒体都需要登录
- 网页响应带有禁止嵌入、禁止 MIME 嗅探和限制脚本来源的安全头
- 开启账号保护时，媒体响应使用 no-store，避免照片留在浏览器共享缓存中
- 旧版较低迭代次数的密码会在成功登录后自动升级

macOS App 可在 `⌘, → 安全` 中配置账号保护。App 使用每次启动随机生成、通过子进程环境传递且不写入磁盘的管理令牌调用本地 API；网页不会获得该令牌，也不能修改账号密码。

CLI 模式可使用隐藏输入配置，密码不会出现在命令行历史中：

```bash
./build/native/astrolibrary auth --username admin
```

关闭账号保护：

```bash
./build/native/astrolibrary auth --disable
```

修改后需要重启 AstroLibrary。密码长度为 8–1024 个字符。关闭保护不会删除已经保存的密码哈希，只会停止登录要求。

如果忘记密码：

1. 退出 AstroLibrary。
2. 打开 `~/Library/Application Support/AstroLibrary/settings.json`。
3. 删除其中的 `auth` 配置块，或把 `enabled` 改为 `false`。
4. 重新启动应用，账号保护会恢复为默认关闭。

## 访问权限

Seestar 沙盒目录可能受 macOS 隐私权限保护。应用不会绕过系统权限；应通过 macOS App 的“通用”设置或 CLI 浏览器模式的文件夹选择器授予 `MyWorks` 访问权限。

## Cloudflare R2

R2 上传使用 Cloudflare 的固定 S3 API 地址 `https://<ACCOUNT_ID>.r2.cloudflarestorage.com` 和 SigV4 `auto` 区域。Account ID 只能是 32 位十六进制字符，应用不接受自定义上传主机；公开地址只接受没有账号、查询参数和路径的 HTTPS 根地址，以降低 SSRF 和凭据误发风险。

R2 Secret Access Key 会以明文保存在本机 `settings.json`，因为 S3 请求签名需要可恢复密钥。文件使用原子替换和 `0600` 权限，Secret 不会通过设置 API 回显，也不会写入映射或服务日志。仍应使用只授权目标存储桶的独立 R2 凭据，不要复用其他服务的密钥；怀疑泄露时应立即在 Cloudflare 撤销并重新创建。

上传前会通过 ImageIO 在内存中生成独立的降采样 JPEG，并删除 JPEG 中可能包含 EXIF/GPS/XMP/IPTC 的元数据段。原始照片、FIT/FITS 和 `_sub` 逐帧不进入上传候选。R2 桶默认私有；填写公开访问地址只会构建 URI，不会替用户开启存储桶公开访问。

## 设置文件

```text
~/Library/Application Support/AstroLibrary/settings.json
~/Library/Application Support/AstroLibrary/catalog.json
~/Library/Application Support/AstroLibrary/r2-mappings.json
```

从旧版本升级时，AstroLibrary 会依次读取旧的 `~/Library/Application Support/AstroShelf/` 和更早的 `~/Library/Application Support/SeeStarPic/` 配置，并以原子方式写入新的 AstroLibrary 目录。旧文件不会被删除；R2 映射也会迁移，已配置的对象前缀保持不变。Bundle ID 已更新为 `com.astrolibrary.local`，因此 macOS 可能要求用户重新授予一次 `MyWorks` 文件夹访问权限。

`settings.json` 保存源目录、导出目录、修图目录、端口、导出范围、目标合并开关、网页保护配置和 R2 凭据；`catalog.json` 保存目标标签、评分和后期工作流；`r2-mappings.json` 保存本地图片与远端 URI 的映射。JSON 文件都使用原子替换和 `0600` 权限写入，预览缓存目录使用 `0700`，属于本机用户数据，不应提交到 Git。若主设置或 R2 映射文件损坏，服务会拒绝启动而不是静默关闭保护或覆盖映射。
