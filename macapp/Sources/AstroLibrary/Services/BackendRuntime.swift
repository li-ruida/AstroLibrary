import Foundation
import Darwin

struct BackendRuntime: Sendable {
    let executable: URL
    let version: String

    static let requiredResources = ["web/index.html", "web/app.js", "web/styles.css"]

    static func detect(executable: URL, resources: URL, timeout: TimeInterval = 5) async throws -> BackendRuntime {
        try await Task.detached(priority: .userInitiated) {
            let files = FileManager.default
            let missing = requiredResources.filter { !files.isReadableFile(atPath: resources.appendingPathComponent($0).path) }
            guard missing.isEmpty else {
                throw ServerLaunchError.environment("应用资源缺失或不可读：\(missing.joined(separator: "、"))。请重新构建或安装完整的 AstroLibrary.app。")
            }
            guard files.isExecutableFile(atPath: executable.path) else {
                throw ServerLaunchError.environment("原生服务缺失或不可执行：\(executable.path)。请重新构建或安装完整的 AstroLibrary.app。")
            }
            do {
                let output = try probe(executable, directory: resources, timeout: timeout)
                let report = try JSONDecoder().decode(Report.self, from: Data(output.utf8))
                guard report.ok, report.protocolVersion == 1, report.backend == "cpp" else {
                    throw ProbeError.failed("原生服务版本不兼容")
                }
                return BackendRuntime(executable: executable, version: report.version)
            } catch {
                throw ServerLaunchError.environment("原生服务自检失败：\(error.localizedDescription)。请重新构建或安装完整的 AstroLibrary.app。")
            }
        }.value
    }

    private struct Report: Decodable {
        let ok: Bool
        let protocolVersion: Int
        let version: String
        let backend: String
    }

    private enum ProbeError: LocalizedError {
        case failed(String)
        var errorDescription: String? { if case .failed(let message) = self { return message }; return nil }
    }

    private static func probe(_ executable: URL, directory: URL, timeout: TimeInterval) throws -> String {
        let temporary = FileManager.default.temporaryDirectory.appendingPathComponent("astrolibrary-probe-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: temporary) }
        let output = temporary.appendingPathComponent("output")
        FileManager.default.createFile(atPath: output.path, contents: nil, attributes: [.posixPermissions: 0o600])
        let log = try FileHandle(forWritingTo: output)
        defer { try? log.close() }
        let task = Process()
        task.executableURL = executable
        task.arguments = ["--self-test"]
        task.currentDirectoryURL = temporary
        task.standardInput = FileHandle.nullDevice
        task.standardOutput = log
        task.standardError = log
        try task.run()
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        while task.isRunning && ProcessInfo.processInfo.systemUptime < deadline { Thread.sleep(forTimeInterval: 0.025) }
        if task.isRunning {
            task.terminate()
            let grace = ProcessInfo.processInfo.systemUptime + 0.25
            while task.isRunning && ProcessInfo.processInfo.systemUptime < grace { Thread.sleep(forTimeInterval: 0.025) }
            if task.isRunning { kill(task.processIdentifier, SIGKILL) }
            task.waitUntilExit()
            throw ProbeError.failed("检测超时，已停止原生服务探针")
        }
        let reader = try FileHandle(forReadingFrom: output)
        defer { try? reader.close() }
        let length = try reader.seekToEnd()
        try reader.seek(toOffset: length > 4096 ? length - 4096 : 0)
        let text = String(decoding: try reader.readToEnd() ?? Data(), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        guard task.terminationStatus == 0 else {
            throw ProbeError.failed(text.isEmpty ? "进程退出码 \(task.terminationStatus)" : String(text.suffix(600)))
        }
        return text
    }

}
