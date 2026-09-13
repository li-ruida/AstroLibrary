# 开发与打包

## 项目结构

```text
Package.swift                 SwiftPM App 工程
macapp/Sources/                SwiftUI / WebKit、设置与内置服务检测
macapp/Resources/              AppIcon 资产目录
native/src/core.*              文件路径、原子写入、密码、导入复制等公共实现
native/src/fits.cpp            FIT 解码、显示、SQLite 与内存缓存
native/src/library.cpp         图库、SUB、修图关联、评分和标签装饰
native/src/state.*             设置、目录、认证和元数据状态
native/src/jobs.cpp            缓存、导入、导出、清理和 R2 后台任务
native/src/server.cpp          受保护的回环 HTTP API
native/src/main.cpp            CLI：serve / export / auth / render
native/src/platform.mm         macOS 废纸篓、HTTPS、ImageIO JPEG 桥接
native/vendor/                 固定版本第三方源码和许可证
native/Makefile                C++ 增量构建，产物在 build/native/
web/                          图库页面与交互
script/build_native.sh        构建原生 CLI
script/build_and_run.sh       构建 App、启动、日志、验证与打包
tests/test_native_backend.py   原生可执行文件的黑盒 API 与固定 FIT 像素对照
tests/test_native_fits.py      通过原生 CLI 检查 FIT 显示行为
tests/fixtures/               迁移前固定的像素、接口与旧缓存兼容样本
tests/                        测试工具、前端测试和原生性能测量
docs/                         项目文档
```

## 构建与运行

需要 macOS 14+。App 构建需要完整 Xcode；仅构建 CLI 可使用 Command Line Tools。运行成品 App/CLI 无需 Python 或 Homebrew。

```bash
./script/build_native.sh                         # C++ CLI
./build/native/astrolibrary serve --port 8899 --open-browser
./script/build_and_run.sh                        # 构建并启动 App
./script/build_and_run.sh --verify               # 启动并检查本地服务
./script/build_and_run.sh --package              # 只打包 App 和 DMG
```

App 脚本先停止旧实例，再编译；`--package` 不启动 App。另支持 `--debug`、`--logs` 和 `--telemetry`。默认端口来自设置，未配置时为 `8765`；CLI `--port` 可临时覆盖。本地工具的 Run 动作可指向同一构建脚本，工具配置不属于源码。

SwiftUI / WebKit 界面程序为 `Contents/MacOS/AstroLibrary`，C++ 后端为 `Contents/MacOS/astrolibrary-backend`。两者必须使用不同名称，兼容 macOS 不区分大小写的磁盘。Objective-C++ 桥接目录选择、废纸篓、NSURLSession HTTPS 和 ImageIO JPEG 编码。

后端只链接 macOS 系统框架、SQLite、zlib 和 CommonCrypto；JSON/HTTP 从仓库内固定头文件编译，构建不下载依赖。许可证见 [THIRD_PARTY.md](../native/THIRD_PARTY.md)。

## 测试与启动检测

```bash
./script/build_native.sh
./build/native/astrolibrary --self-test
python3 -m unittest discover -s tests -v
node --check web/app.js
node tests/test_gallery.cjs
swift test --scratch-path build/swift
bash -n script/build_and_run.sh
```

Python 仅是标准库测试驱动，直接运行发布的 C++ 可执行文件。接口、像素及旧缓存兼容性使用固定合成样本，覆盖范围见 [tests/fixtures/README.md](../tests/fixtures/README.md)。不要用待测实现自动覆盖期望值。原生 HTTP 测试以不存在的 `PATH` 启动服务，在临时图库和缓存上运行。

App 的 `BackendRuntime` 检查 Web 资源和后端可执行权限，在 5 秒内运行 `--self-test`，验证 SHA-256、SQLite、PNG 编码及协议版本 1 / `backend=cpp`。检测不读取个人设置；超时终止。启动后用每次启动的随机令牌请求 `/api/app/health`，避免误认其他服务；异常退出显示日志及重试入口。Swift 测试覆盖真实服务、资源缺失、协议错误、损坏响应和超时。

R2 配置、密钥不回显和 HEAD/PUT 签名有本地对照测试；真实云端上传和特定磁盘故障仍需对应环境验证。历史性能测量与复测方法集中在 [PERFORMANCE.md](PERFORMANCE.md)。

## 打包

产物为 `dist/AstroLibrary.app` 和 `dist/AstroLibrary.dmg`，按构建机架构生成。App 只包含界面、后端、Web 资源及第三方许可，不包含 Python 测试工具。

```bash
./script/build_and_run.sh --package
codesign --verify --deep --strict dist/AstroLibrary.app
hdiutil verify dist/AstroLibrary.dmg
```

当前使用 ad hoc 签名，尚未配置 Developer ID、硬化运行时及公证流程。现有打包验证针对 arm64；不能据此宣称其他架构或正式分发已验证。

图标母版为 `packaging/AppIcon.png`。资产目录保留 macOS 的 10 个尺寸/倍率槽位，相同实际分辨率共用 PNG；`Contents.json` 指向对应文件。

## 清理本地文件

没有构建任务运行时，可以删除 `build/swift/` 释放 Swift 编译缓存；下次构建会自动重建，首次编译会更慢。`build/native/` 包含 CLI 和增量编译文件，删除后先运行 `./script/build_native.sh`。其中性能对照用的旧二进制不一定能由当前源码重建，应按需保留。

`dist/` 是打包产物，确认不再使用其中的 App 和 DMG 后可删除，再通过 `./script/build_and_run.sh --package` 生成。`output/` 中的临时导出、草稿和审阅 ZIP 完成用途后可删除；清理前确认正式资源已放入源码目录。`.gitignore` 只防止提交，不会自动释放磁盘空间。

保留 `tests/`、固定对照数据、性能报告、真实页面截图，以及 `packaging/AppIcon.png` 图标母版和 App 资产目录；这些文件分别用于验证、文档和打包。Python 文件仅用于开发测试，不会进入 App。

`.gitignore` 不会移除 Git 历史中已有的大文件。历史里的 Siril/FIT 图像若需彻底移除，应先确认照片备份及历史保留范围，再单独处理历史；不要直接删除 `.git/objects/`。

## 日志

App 内部本地服务日志位于：

```text
~/Library/Logs/AstroLibrary/server.log
```

开发服务器前台运行时，日志会直接输出到终端。

## FIT 缓存与接口

FIT 持久化缓存由 C++ `FitsCache` 接入。测试以 `--data-dir`、`--cache-dir`、`--web-root` 隔离设置、缓存和资源。继续使用 `fits-v1.sqlite3` 与头/采样版本 1、显示版本 2；缓存键包含文件路径、设备/节点、大小和修改/变更时间。像素缓存采用有界 JSON + zlib float64 数据，不使用可执行序列化。升级解码或显示规则时必须提升对应键中的版本。

缓存维护 API：`GET /api/fits-cache` 返回缓存占用与任务状态；`POST /api/fits-cache/clear`、`rebuild`、`cancel` 使用既有本机 Host、认证及 CSRF/App token 校验。操作立即返回，任务后台串行执行，状态读取不等待 FIT 解码。清空通过 SQLite DELETE/VACUUM 处理缓存记录，不删除源文件或设置目录。

自动建立与手动重建缓存在遍历阶段跳过名称以 `_sub` 结尾的目录（不区分大小写，包含嵌套目录），避免枚举和预解析其中的单帧。图库扫描仍包含 SUB，直接打开 SUB 预览时照常按需解析并缓存。

缓存记录数和字节数按当前 SQLite 连接增量维护，写入及淘汰在同一个 `BEGIN IMMEDIATE` 事务中进行。`PRAGMA data_version` 变化时重新统计，兼容其他进程写入与旧缓存格式；仅超出容量时遍历 LRU 索引淘汰。图库扫描同时生成完整快照与 SUB 摘要，首页只复制摘要，目标详情仅复制指定目标。性能测量和复测脚本见 [PERFORMANCE.md](PERFORMANCE.md)。

`POST /api/fits-cache/directory` 仅接受本机 App token，JSON `directory` 为所选父目录的绝对路径，空字符串恢复默认。设置以 `fitsCacheDirectory` 保存，自选位置使用 `AstroLibrary-FIT-Cache` 子目录。切换由缓存任务锁和设置锁保护，先打开并验证新缓存，再原子持久化设置；失败保留当前配置和缓存连接。成功后清除内存派生数据并更新 ETag 世代，旧磁盘缓存保留。GET 状态同时返回 `configuredDirectory`（空表示默认）和 `defaultDirectory`。原有通用设置更新不覆盖此字段。

## 图库与导入目录

“精选成片目录”沿用 `editedSource` 和 `galleryMode=edited`，兼容旧修图成果配置。扫描不会因原始 `source` 不可用而提前返回，精选目录可独立使用；没有原图可关联时，按相对父目录或根目录文件名归类。前端基于当前显示范围生成目标、设备和标签筛选，并依据所选目录显示访问提示。

照片评分通过 `POST /api/rating` 保存，字段为 `id`、`isEdited`、`scope`（列表返回的 `ratingScope`）和整数 `rating`（0 清除，1–5 星）。端点沿用认证及 CSRF/App token 保护，拒绝越界、已失效的目录和非法评分。`catalog.json.photoRatings` 使用目录和相对路径的 SHA-256 作为键，原子写入且不修改图像。列表和 SUB 明细动态附加评分，打分无需重新扫描或解析 FIT。前端先做评分筛选再选版本，显式评分排序优先于格式，未评分置后。

`libraryPath` 是持久化的图库目录；内部 `State.settings["source"]` 和 JSON `source` 保留为兼容别名，均指图库。旧配置将原 `source` 映射为图库并保留展示位置，不移动照片；CLI `serve --library`（旧 `--source` 仍有效）可覆盖图库。选择目录原样使用，不自动下钻到 MyWorks。启动不探测 Seestar，离线图库路径不会被其他目录替换。

`seestarSource` 是独立可选导入来源。`POST /api/seestar/detect` 只返回候选项；`POST /api/seestar/import` 启动后台复制，`GET /api/seestar/import` 返回状态、计数和错误。端点沿用 Host、认证及 CSRF/App token 校验。导入包含 FIT/照片/SUB，保留相对路径，跳过同名文件。文件先写临时文件，再安装到图库；原文件只读。导入、导出和 JPG 清理共享操作锁；导入时拒绝更改图库或导入来源。导入结束（含部分失败）使扫描缓存失效。

`destination` 只用于图库导出副本。目录相同或嵌套的限制在实际导出时检查，不阻止用户将旧导出目录设置成图库。FIT 预览、缓存重建、R2 媒体读取及 JPG 清理都使用图库根目录。
