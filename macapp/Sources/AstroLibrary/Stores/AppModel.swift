import Foundation

@MainActor
final class AppModel: ObservableObject {
    enum ServerState: Equatable {
        case idle
        case starting
        case ready
        case failed(String)
    }

    @Published private(set) var serverState: ServerState = .idle
    @Published private(set) var launchMessage = "正在检查运行环境…"
    @Published private(set) var serverURL: URL?
    @Published private(set) var settings: AppSettings?
    @Published var reloadToken = 0
    @Published var notice = ""
    @Published var isWorking = false
    @Published private(set) var cacheStatus: CacheStatus?
    @Published private(set) var cacheActionPending = false
    @Published private(set) var cacheError = ""
    @Published private(set) var importStatus: SeestarImportStatus?
    @Published private(set) var importPending = false
    @Published private(set) var importError = ""
    @Published private(set) var seestarCandidates: [SeestarCandidate] = []

    @Published private(set) var seestarCleanupStatus: SeestarCleanupStatus?
    @Published private(set) var seestarCleanupPending = false
    @Published private(set) var seestarCleanupError = ""

    let appToken = UUID().uuidString
    private let server = LocalServerController()
    private var importMonitor: Task<Void, Never>?

    func start() async {
        guard serverState == .idle else { return }
        serverState = .starting
        launchMessage = "正在检查原生服务和应用资源…"
        let port = savedPort()
        let url = URL(string: "http://127.0.0.1:\(port)")!
        do {
            try await server.start(port: port, appToken: appToken)
            launchMessage = "环境检查通过，正在启动本地照片库…"
            try await waitUntilReady(url)
            serverURL = url
            serverState = .ready
            settings = try await api.settings()
        } catch {
            server.stop()
            serverURL = nil
            serverState = .failed(error.localizedDescription)
        }
    }

    func retryStart() async {
        server.stop()
        serverURL = nil
        serverState = .idle
        await start()
    }

    func reloadGallery() {
        reloadToken += 1
    }

    func loadSettings() async {
        guard serverURL != nil else { return }
        isWorking = true
        defer { isWorking = false }
        do {
            settings = try await api.settings()
            notice = ""
        } catch {
            notice = error.localizedDescription
        }
    }

    func saveSettings(_ update: SettingsUpdate) async -> Bool {
        isWorking = true
        defer { isWorking = false }
        do {
            let previousPort = settings?.port
            let previousLibrary = settings?.libraryPath
            settings = try await api.save(update)
            reloadGallery()
            notice = previousPort != update.port ? "设置已保存；新端口会在下次启动时生效。" : "设置已保存。"
            if previousLibrary != settings?.libraryPath {
                notice += "已安排后台建立图库缓存，可在“缓存”页查看或停止。"
            }
            return true
        } catch {
            notice = error.localizedDescription
            return false
        }
    }

    func updateAuth(enabled: Bool, username: String, password: String) async -> Bool {
        isWorking = true
        defer { isWorking = false }
        do {
            settings = try await api.updateAuth(AuthUpdate(enabled: enabled, username: username, password: password))
            notice = enabled ? "账号保护已更新。" : "账号保护已关闭。"
            reloadGallery()
            return true
        } catch {
            notice = error.localizedDescription
            return false
        }
    }

    func testR2(_ update: R2Update) async {
        isWorking = true
        defer { isWorking = false }
        do {
            let response = try await api.testR2(update)
            notice = response.message ?? "R2 连接成功。"
        } catch {
            notice = error.localizedDescription
        }
    }

    func syncR2() async {
        isWorking = true
        defer { isWorking = false }
        do {
            let response = try await api.syncR2()
            notice = response.started == true ? "R2 同步已开始。" : "R2 同步已经在运行。"
            settings = try await api.settings()
        } catch {
            notice = error.localizedDescription
        }
    }

    private var api: NativeAPI {
        NativeAPI(baseURL: serverURL!, appToken: appToken)
    }

    func detectSeestar() async {
        guard serverURL != nil, !importPending else { return }
        importPending = true
        defer { importPending = false }
        do {
            seestarCandidates = try await api.detectSeestar().candidates
            importError = seestarCandidates.contains(where: { $0.accessible && $0.hasEntries }) ? "" : "未发现可用的 Seestar 作品目录，可以手动选择。"
        } catch { importError = error.localizedDescription }
    }

    func saveSeestarSource(_ path: String, startImport: Bool = false) async {
        guard serverURL != nil, !importPending else { return }
        importPending = true
        defer { importPending = false }
        do {
            settings = try await api.saveSeestarSource(path)
            if startImport { _ = try await api.startSeestarImport() }
            importStatus = try await api.seestarImportStatus()
            importError = ""
            if startImport {
                importMonitor?.cancel()
                importMonitor = Task { [weak self] in
                    while !Task.isCancelled {
                        guard let self else { return }
                        if self.importStatus?.running != true { self.reloadGallery(); return }
                        do { try await Task.sleep(for: .seconds(1)) } catch { return }
                        await self.refreshImportStatus()
                    }
                }
            }
        } catch { importError = error.localizedDescription }
    }

    func refreshImportStatus() async {
        guard serverURL != nil, !importPending else { return }
        do {
            let result = try await api.seestarImportStatus()
            if !Task.isCancelled && !importPending {
                let completed = result.finished && importStatus?.finished != true
                importStatus = result
                if completed { reloadGallery() }
            }
        } catch {
            if !Task.isCancelled { importError = error.localizedDescription }
        }
    }

    func refreshSeestarCleanupStatus() async {
        guard serverURL != nil, !seestarCleanupPending else { return }
        do {
            let result = try await api.seestarCleanupStatus()
            if !Task.isCancelled && !seestarCleanupPending { seestarCleanupStatus = result }
        } catch {
            if !Task.isCancelled { seestarCleanupError = error.localizedDescription }
        }
    }

    func seestarCleanupAction(_ action: String, token: String = "") async {
        guard !seestarCleanupPending else { return }
        seestarCleanupPending = true
        seestarCleanupError = ""
        defer { seestarCleanupPending = false }
        do { seestarCleanupStatus = try await api.seestarCleanupAction(action, token: token) }
        catch { seestarCleanupError = error.localizedDescription }
    }

    func refreshCacheStatus() async {
        guard serverURL != nil, !cacheActionPending else { return }
        do {
            let response = try await api.cacheStatus()
            if !Task.isCancelled && !cacheActionPending {
                cacheStatus = response
                cacheError = ""
            }
        } catch {
            if !Task.isCancelled { cacheError = error.localizedDescription }
        }
    }

    func runCacheAction(_ action: String) async {
        guard serverURL != nil, !cacheActionPending else { return }
        cacheActionPending = true
        defer { cacheActionPending = false }
        do {
            _ = try await api.cacheAction(action)
            cacheStatus = try await api.cacheStatus()
            cacheError = ""
        } catch {
            cacheError = error.localizedDescription
        }
    }

    func setCacheDirectory(_ directory: String) async -> String? {
        guard serverURL != nil, !cacheActionPending else { return "请等待当前操作结束" }
        cacheActionPending = true
        defer { cacheActionPending = false }
        do {
            _ = try await api.setCacheDirectory(directory)
            cacheStatus = try await api.cacheStatus()
            cacheError = ""
            return nil
        } catch {
            return error.localizedDescription
        }
    }

    private func waitUntilReady(_ url: URL) async throws {
        let healthURL = url.appendingPathComponent("api/app/health")
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 1
        configuration.timeoutIntervalForResource = 1
        configuration.waitsForConnectivity = false
        let session = URLSession(configuration: configuration)
        defer { session.invalidateAndCancel() }

        // Give the local interpreter a moment to bind before the first request.
        try await Task.sleep(for: .milliseconds(100))
        for _ in 0..<24 {
            if server.process?.isRunning != true {
                throw ServerLaunchError.stopped(server.failureDetail())
            }
            var request = URLRequest(url: healthURL)
            request.setValue(appToken, forHTTPHeaderField: "X-AstroLibrary-App-Token")
            request.cachePolicy = .reloadIgnoringLocalAndRemoteCacheData
            request.timeoutInterval = 1
            if let (_, response) = try? await session.data(for: request),
               let http = response as? HTTPURLResponse,
               http.statusCode == 200 {
                return
            }
            try await Task.sleep(for: .milliseconds(250))
        }
        throw ServerLaunchError.stopped("健康检查超时，请确认端口没有被占用。")
    }

    private func savedPort() -> Int {
        let path = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/AstroLibrary/settings.json")
        guard let data = try? Data(contentsOf: path),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let port = object["port"] as? Int,
              1024...65535 ~= port else { return 8765 }
        return port
    }
}
