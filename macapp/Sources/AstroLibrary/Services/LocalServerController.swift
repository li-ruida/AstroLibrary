import AppKit
import Foundation

@MainActor
final class LocalServerController {
    private(set) var process: Process?
    private var terminationObserver: NSObjectProtocol?
    private var launchLogOffset: UInt64 = 0
    static var logURL: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/AstroLibrary/server.log")
    }

    deinit {
        if let terminationObserver {
            NotificationCenter.default.removeObserver(terminationObserver)
        }
        process?.terminate()
    }

    func start(port: Int, appToken: String) async throws {
        guard process?.isRunning != true else { return }
        guard let resources = serverResourcesURL(), let executable = serverExecutableURL() else {
            throw ServerLaunchError.missingResources
        }
        let runtime = try await BackendRuntime.detect(executable: executable, resources: resources)

        let task = Process()
        task.executableURL = runtime.executable
        task.arguments = [
            "serve", "--web-root", resources.appendingPathComponent("web").path,
            "--host", "127.0.0.1", "--port", String(port),
        ]
        var environment = ProcessInfo.processInfo.environment
        environment["ASTROLIBRARY_APP_TOKEN"] = appToken
        task.environment = environment
        task.currentDirectoryURL = resources

        let logURL = Self.logURL
        try FileManager.default.createDirectory(
            at: logURL.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        if !FileManager.default.fileExists(atPath: logURL.path) {
            FileManager.default.createFile(atPath: logURL.path, contents: nil)
        }
        let log = try FileHandle(forWritingTo: logURL)
        launchLogOffset = try log.seekToEnd()
        task.standardOutput = log
        task.standardError = log
        task.terminationHandler = { _ in try? log.close() }

        do { try task.run() }
        catch { try? log.close(); throw error }
        process = task
        terminationObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.willTerminateNotification,
            object: nil,
            queue: .main
        ) { [weak task] _ in
            task?.terminate()
        }
    }

    func stop() {
        process?.terminate()
        process = nil
        if let terminationObserver { NotificationCenter.default.removeObserver(terminationObserver) }
        terminationObserver = nil
    }

    func failureDetail() -> String {
        let code = process.map { $0.isRunning ? "" : "退出码 \($0.terminationStatus)。" } ?? ""
        guard let reader = try? FileHandle(forReadingFrom: Self.logURL) else { return code }
        defer { try? reader.close() }
        guard let end = try? reader.seekToEnd(), end > launchLogOffset else { return code }
        try? reader.seek(toOffset: max(launchLogOffset, end > 1600 ? end - 1600 : 0))
        let detail = String(decoding: (try? reader.readToEnd()) ?? Data(), as: UTF8.self)
        return code + detail.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func serverResourcesURL() -> URL? {
        if Bundle.main.bundleURL.pathExtension == "app" {
            return Bundle.main.resourceURL?.appendingPathComponent("AstroLibrary")
        }
        return URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
    }

    private func serverExecutableURL() -> URL? {
        if Bundle.main.bundleURL.pathExtension == "app" {
            return Bundle.main.executableURL?.deletingLastPathComponent().appendingPathComponent("astrolibrary-backend")
        }
        return URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
            .appendingPathComponent("build/native/astrolibrary")
    }

}

enum ServerLaunchError: LocalizedError {
    case missingResources
    case environment(String)
    case stopped(String)

    var errorDescription: String? {
        switch self {
        case .missingResources:
            return "应用包中缺少 AstroLibrary 服务资源。"
        case .environment(let detail):
            return "运行环境检查未通过。\n\(detail)"
        case .stopped(let detail):
            return "本地服务未能启动。\(detail)"
        }
    }
}
