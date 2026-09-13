# 开源发布检查

## 准备源码

公开内容应包含 `Package.swift`、`macapp/`、`native/`、`web/`、`script/`、`packaging/`、`tests/` 和项目文档。第三方头文件、许可证、测试对照数据、图标母版及文档截图是项目的一部分，不应作为缓存删除。

以下内容已通过 `.gitignore` 排除：

- `build/`、`dist/`、`output/`、Swift 编译缓存和测试日志。
- `.codex/` 等本机工具配置、环境变量文件、SSH/AWS 配置和证书私钥。
- 任意层级的 `settings.json`、`catalog.json`、`r2-mappings.json` 及其备份；明确命名的 `.example` 模板可以提交，但必须使用示例值。
- FIT/FITS、常见照片格式、Siril 中间文件及本地归档。仅放行 `packaging/AppIcon.png`、App 资产目录中的 PNG，以及 `docs/screenshots/` 下经过检查的 JPG/PNG。

忽略规则只影响未被跟踪的文件，不能自动删除已有提交或历史对象中的内容。不要用 `git add -f` 绕过这些规则。新增图片资源时，应只放行需要发布的具体资源目录。

发布前核对当前文件与暂存区：

```bash
git status --short
git add --dry-run --all
git ls-files -ci --exclude-standard
git diff --check
git diff --cached --stat
```

`git ls-files -ci --exclude-standard` 应没有输出；它能发现已被跟踪、但按当前规则应排除的文件。`git add --dry-run` 只展示候选操作，不提交源码。源码尚有未提交改动时，直接推送分支或使用 `git archive HEAD` 都不会包含这些改动。

## 敏感信息与资源

检查真实 API Key、R2 凭据、密码、令牌、带凭据的 URL、个人路径、邮箱和内部服务地址。对源码、暂存区以及准备公开的完整提交历史分别检查；提交作者和提交者邮箱也是公开内容。自动模式匹配需要人工复核，不能证明所有敏感信息都已排除。

测试中的 `native-password`、`legacy-password`、`secret-value-123456789` 和示例 R2 标识是合成数据。固定 FIT 像素及缓存样本位于 `tests/fixtures/`，用途见该目录的说明，不连接真实云账号。

文档截图会公开其可见内容，包括拍摄目标、日期和设备型号。添加截图时同时检查画面及 EXIF/GPS/XMP/IPTC 元数据，避免出现账号、密钥、个人目录或定位数据。图标和截图应有明确的来源与发布授权。

运行时的个人设置和图库不属于源码。不要把 `~/Library/Application Support/AstroLibrary/`、照片目录或包含这些数据的备份打包发布。App 内可能保存明文 R2 Secret，具体保护方式见 [配置与安全](SECURITY.md)。

## Git 历史

通过以下命令确认历史和对象占用：

```bash
git log --oneline --decorate
git for-each-ref --format='%(refname) %(objectname:short)'
git count-objects -vH
git fsck --full
```

正式分支已只有一个提交时，不需要再次合并历史。本地工具可能创建 `refs/codex/` 检查点；普通的明确分支推送不包含这些引用，发布时不要使用 `git push --mirror`。不要把 `.git` 或整个工作目录压缩后作为源码包发布。

若旧提交包含需要移除的个人身份或旧实现，可在确认发布用姓名、邮箱并备份本地历史后，将审阅过的当前源码整理为一个新的初始提交。仅修改 `git config user.email` 不会改变旧提交里的邮箱；`.mailmap` 也不会擦除原始提交信息。合并历史和压缩 Git 对象是不同操作，压缩不能消除仍被引用的秘密。

删除历史引用或执行垃圾回收前，应保留仓库外的恢复备份并确认没有并发 Git 写入。备份同样可能包含私人照片及旧凭据，不应公开。

## 许可证与打包

项目许可证需要由维护者选定。第三方依赖的许可证与版本已列在 [THIRD_PARTY.md](../native/THIRD_PARTY.md)，分发时保留 `native/vendor/` 中对应许可文件。项目自身许可证不能替代第三方许可说明。

发布源码前运行 [开发文档](DEVELOPMENT.md) 中的回归检查。App 和 DMG 应作为独立发布附件，不提交到源码仓库；从干净的源码目录重新构建，避免分发开发机上的旧产物。
