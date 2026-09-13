import XCTest
@testable import AstroLibrary

final class BackendRuntimeTests: XCTestCase {
    private var root: URL!
    private var project: URL {
        URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    }
    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent("backend-test-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
    }
    override func tearDownWithError() throws { try FileManager.default.removeItem(at: root) }
    private func executable(_ text: String) throws -> URL {
        let path = root.appendingPathComponent("helper")
        try text.write(to: path, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: path.path)
        return path
    }
    private func failure(_ executable: URL, resources: URL? = nil, timeout: TimeInterval = 5) async -> String {
        do {
            _ = try await BackendRuntime.detect(executable: executable, resources: resources ?? project, timeout: timeout)
            XCTFail("Expected a failed check")
            return ""
        } catch { return error.localizedDescription }
    }
    func testRealNativeBackend() async throws {
        let helper = project.appendingPathComponent("build/native/astrolibrary")
        let runtime = try await BackendRuntime.detect(executable: helper, resources: project)
        XCTAssertEqual(runtime.executable, helper)
        XCTAssertEqual(runtime.version, "0.8.0")
    }
    func testMissingExecutableOffersReinstall() async {
        let error = await failure(root.appendingPathComponent("missing"))
        XCTAssertTrue(error.contains("原生服务缺失"), error)
        XCTAssertTrue(error.contains("重新构建或安装"), error)
    }
    func testMissingWebResources() async {
        let error = await failure(root.appendingPathComponent("missing"), resources: root)
        XCTAssertTrue(error.contains("web/index.html"), error)
    }
    func testNonzeroExitIncludesDiagnostic() async throws {
        let helper = try executable("#!/bin/sh\necho incompatible-architecture >&2\nexit 1\n")
        let error = await failure(helper)
        XCTAssertTrue(error.contains("incompatible-architecture"), error)
    }
    func testWrongProtocolRejected() async throws {
        let helper = try executable("#!/bin/sh\necho '{\"ok\":true,\"protocolVersion\":9,\"version\":\"9\",\"backend\":\"cpp\"}'\n")
        let error = await failure(helper)
        XCTAssertTrue(error.contains("版本不兼容"), error)
    }
    func testInvalidProbeRejected() async throws {
        let helper = try executable("#!/bin/sh\necho not-json\n")
        let error = await failure(helper)
        XCTAssertTrue(error.contains("自检失败"), error)
    }
    func testHungProbeIsKilled() async throws {
        let helper = try executable("#!/bin/sh\ntrap '' TERM\nwhile :; do :; done\n")
        let start = Date()
        let error = await failure(helper, timeout: 0.3)
        XCTAssertTrue(error.contains("检测超时"), error)
        XCTAssertLessThan(Date().timeIntervalSince(start), 2)
    }
}
