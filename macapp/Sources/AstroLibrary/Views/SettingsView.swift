import SwiftUI

struct SettingsView: View {
    @ObservedObject var model: AppModel
    @State private var draft: SettingsDraft?

    var body: some View {
        TabView {
            GeneralSettings(draft: binding, chooseFolder: chooseFolder, save: save)
                .tabItem { Label("通用", systemImage: "slider.horizontal.3") }

            CacheSettingsView(model: model)
                .tabItem { Label("缓存", systemImage: "internaldrive") }

            SeestarSettingsView(model: model)
                .tabItem { Label("Seestar 导入", systemImage: "square.and.arrow.down") }

            CloudSettings(
                draft: binding,
                test: { Task { await model.testR2(binding.wrappedValue.r2Update) } },
                sync: {
                    Task {
                        let value = binding.wrappedValue
                        if await model.saveSettings(value.settingsUpdate) {
                            if !value.secretAccessKey.isEmpty { draft?.secretAccessKey = "" }
                            await model.syncR2()
                        }
                    }
                },
                save: save,
                isWorking: model.isWorking
            )
            .tabItem { Label("R2 同步", systemImage: "icloud") }

            SecuritySettings(draft: binding, save: saveAuth, isWorking: model.isWorking)
                .tabItem { Label("安全", systemImage: "lock.shield") }
        }
        .frame(width: 620, height: 520)
        .overlay(alignment: .bottom) {
            if !model.notice.isEmpty {
                Text(model.notice)
                    .font(.callout)
                    .foregroundStyle(model.notice.contains("失败") || model.notice.contains("错误") ? .red : .secondary)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .background(.regularMaterial, in: Capsule())
                    .padding(.bottom, 10)
            }
        }
        .task {
            await model.loadSettings()
            resetDraft()
        }
        .onChange(of: model.settings) { _, _ in resetDraft() }
    }

    private var binding: Binding<SettingsDraft> {
        Binding(
            get: { draft ?? SettingsDraft(settings: model.settings) },
            set: { draft = $0 }
        )
    }

    private func resetDraft() {
        draft = SettingsDraft(settings: model.settings)
    }

    private func chooseFolder(_ kind: FolderKind) {
        var value = binding.wrappedValue
        let current: String
        let title: String
        switch kind {
        case .source:
            current = value.source
            title = "选择图库目录（任意照片文件夹）"
        case .destination:
            current = value.destination
            title = "选择照片导出目录"
        case .edited:
            current = value.editedSource
            title = "选择精选成片目录"
        }
        guard let selected = FolderPicker.choose(title: title, initialPath: current) else { return }
        switch kind {
        case .source: value.source = selected
        case .destination: value.destination = selected
        case .edited: value.editedSource = selected
        }
        draft = value
    }

    private func save() {
        Task {
            let value = binding.wrappedValue
            if await model.saveSettings(value.settingsUpdate) {
                if !value.secretAccessKey.isEmpty { draft?.secretAccessKey = "" }
            }
        }
    }

    private func saveAuth() {
        Task {
            let value = binding.wrappedValue
            if await model.updateAuth(enabled: value.authEnabled, username: value.authUsername, password: value.authPassword) {
                draft?.authPassword = ""
            }
        }
    }
}

private struct GeneralSettings: View {
    @Binding var draft: SettingsDraft
    let chooseFolder: (FolderKind) -> Void
    let save: () -> Void

    var body: some View {
        Form {
            Section("本地目录") {
                pathRow("图库目录", path: $draft.source) { chooseFolder(.source) }
                Text("选择你存放照片的任意文件夹，包含子目录；不要求 MyWorks。Seestar 同步请使用“Seestar 导入”。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                pathRow("导出副本目录", path: $draft.destination) { chooseFolder(.destination) }
                Text("仅在导出副本时使用，不决定图库展示内容。")
                    .font(.caption).foregroundStyle(.secondary)
                pathRow("精选成片目录", path: $draft.editedSource) { chooseFolder(.edited) }
                Text("存放处理并挑选好的 JPG、PNG 或 TIFF，可在图库切换到“精选成片”单独浏览。原始图库离线时仍可使用，建议按目标建立子目录。")
                    .font(.caption).foregroundStyle(.secondary)
            }

            Section("图库") {
                Picker("默认照片版本", selection: $draft.galleryMode) {
                    Text("FIT 优先").tag("auto")
                    Text("原始图像").tag("source")
                    Text("精选成片").tag("edited")
                }
                Picker("导出范围", selection: $draft.exportScope) {
                    Text("全部文件（含 SUB）").tag("all")
                    Text("仅单张成片").tag("single")
                }
                Toggle("合并重复目标", isOn: $draft.mergeTargets)
            }

            Section("本地服务") {
                TextField("端口", value: $draft.port, format: .number)
                Text("服务始终只绑定 127.0.0.1；修改端口后需重新启动 App。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            HStack {
                Spacer()
                Button("保存设置", action: save)
                    .buttonStyle(.borderedProminent)
            }
        }
        .formStyle(.grouped)
        .padding(.top, 12)
    }

    @ViewBuilder
    private func pathRow(_ title: String, path: Binding<String>, choose: @escaping () -> Void) -> some View {
        LabeledContent(title) {
            HStack {
                TextField(title, text: path)
                    .textFieldStyle(.roundedBorder)
                Button("选择…", action: choose)
            }
            .frame(maxWidth: 410)
        }
    }
}

private struct CloudSettings: View {
    @Binding var draft: SettingsDraft
    let test: () -> Void
    let sync: () -> Void
    let save: () -> Void
    let isWorking: Bool

    var body: some View {
        Form {
            Section("Cloudflare R2 预览同步") {
                Toggle("启用 R2 存储", isOn: $draft.r2Enabled)
                Toggle("扫描后自动增量上传", isOn: $draft.r2AutoUpload)
                    .disabled(!draft.r2Enabled)
                Text("仅上传最长边 1600 px、已清理元数据的 JPEG 预览；不会上传 FIT、SUB 逐帧或原图。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("连接") {
                TextField("Account ID", text: $draft.accountId)
                TextField("Bucket", text: $draft.bucket)
                TextField("Access Key ID", text: $draft.accessKeyId)
                SecureField(draft.secretConfigured ? "Secret Access Key（留空以保留）" : "Secret Access Key", text: $draft.secretAccessKey)
                TextField("公开访问地址（可选）", text: $draft.publicBaseURL)
                TextField("对象前缀", text: $draft.keyPrefix)
            }

            HStack {
                Button("测试连接", action: test)
                    .disabled(isWorking)
                Button("立即同步", action: sync)
                    .disabled(isWorking || !draft.r2Enabled)
                Spacer()
                Button("保存设置", action: save)
                    .buttonStyle(.borderedProminent)
                    .disabled(isWorking)
            }
        }
        .formStyle(.grouped)
        .padding(.top, 12)
    }
}

private struct SecuritySettings: View {
    @Binding var draft: SettingsDraft
    let save: () -> Void
    let isWorking: Bool

    var body: some View {
        Form {
            Section("网页账号保护") {
                Toggle("需要账号密码才能访问图库", isOn: $draft.authEnabled)
                TextField("账号", text: $draft.authUsername)
                    .disabled(!draft.authEnabled)
                SecureField(draft.authWasEnabled ? "新密码（留空以保留）" : "密码（至少 8 位）", text: $draft.authPassword)
                    .disabled(!draft.authEnabled)
                Text("原生 App 通过一次性本地管理令牌保存设置；令牌只在本次 App 与服务进程中有效，不写入磁盘。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            HStack {
                Spacer()
                Button(draft.authEnabled ? "保存账号保护" : "关闭账号保护", action: save)
                    .buttonStyle(.borderedProminent)
                    .disabled(isWorking || (draft.authEnabled && draft.authUsername.trimmingCharacters(in: .whitespaces).isEmpty))
            }
        }
        .formStyle(.grouped)
        .padding(.top, 12)
    }
}

enum FolderKind {
    case source, destination, edited
}

struct SettingsDraft {
    var source = ""
    var destination = ""
    var editedSource = ""
    var galleryMode = "auto"
    var exportScope = "all"
    var mergeTargets = true
    var port = 8765
    var authEnabled = false
    var authWasEnabled = false
    var authUsername = ""
    var authPassword = ""
    var r2Enabled = false
    var r2AutoUpload = false
    var accountId = ""
    var bucket = ""
    var accessKeyId = ""
    var secretAccessKey = ""
    var secretConfigured = false
    var publicBaseURL = ""
    var keyPrefix = "astrolibrary/previews"

    init(settings: AppSettings?) {
        guard let settings else { return }
        source = settings.libraryPath
        destination = settings.destination
        editedSource = settings.editedSource
        galleryMode = settings.galleryMode
        exportScope = settings.exportScope
        mergeTargets = settings.mergeTargets
        port = settings.port
        authEnabled = settings.authEnabled
        authWasEnabled = settings.authEnabled
        authUsername = settings.authUsername
        r2Enabled = settings.r2.enabled
        r2AutoUpload = settings.r2.autoUpload
        accountId = settings.r2.accountId
        bucket = settings.r2.bucket
        accessKeyId = settings.r2.accessKeyId
        secretConfigured = settings.r2.secretConfigured
        publicBaseURL = settings.r2.publicBaseUrl
        keyPrefix = settings.r2.keyPrefix
    }

    var r2Update: R2Update {
        R2Update(
            enabled: r2Enabled,
            autoUpload: r2AutoUpload,
            accountId: accountId,
            bucket: bucket,
            accessKeyId: accessKeyId,
            secretAccessKey: secretAccessKey,
            publicBaseUrl: publicBaseURL,
            keyPrefix: keyPrefix
        )
    }

    var settingsUpdate: SettingsUpdate {
        SettingsUpdate(
            libraryPath: source,
            destination: destination,
            editedSource: editedSource,
            galleryMode: galleryMode,
            exportScope: exportScope,
            mergeTargets: mergeTargets,
            port: port,
            r2: r2Update
        )
    }
}
