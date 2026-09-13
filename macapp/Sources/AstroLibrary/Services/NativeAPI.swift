import Foundation

struct NativeAPI {
    let baseURL: URL
    let appToken: String

    func settings() async throws -> AppSettings {
        try await request(path: "/api/settings", method: "GET", body: Optional<Data>.none)
    }

    func save(_ update: SettingsUpdate) async throws -> AppSettings {
        try await request(path: "/api/settings", method: "POST", body: try JSONEncoder().encode(update))
    }

    func updateAuth(_ update: AuthUpdate) async throws -> AppSettings {
        try await request(path: "/api/app/auth", method: "POST", body: try JSONEncoder().encode(update))
    }

    func testR2(_ update: R2Update) async throws -> ActionResponse {
        struct Body: Encodable { let r2: R2Update }
        return try await request(path: "/api/r2/test", method: "POST", body: try JSONEncoder().encode(Body(r2: update)))
    }

    func syncR2() async throws -> ActionResponse {
        try await request(path: "/api/r2/sync", method: "POST", body: Data("{}".utf8))
    }

    func cacheStatus() async throws -> CacheStatus {
        try await request(path: "/api/fits-cache", method: "GET", body: nil)
    }

    func saveSeestarSource(_ path: String) async throws -> AppSettings {
        struct Body: Encodable { let seestarSource: String }
        return try await request(path: "/api/settings", method: "POST", body: try JSONEncoder().encode(Body(seestarSource: path)))
    }

    func detectSeestar() async throws -> SeestarCandidates {
        try await request(path: "/api/seestar/detect", method: "POST", body: Data("{}".utf8))
    }

    func startSeestarImport() async throws -> ActionResponse {
        try await request(path: "/api/seestar/import", method: "POST", body: Data("{}".utf8))
    }

    func seestarImportStatus() async throws -> SeestarImportStatus {
        try await request(path: "/api/seestar/import", method: "GET", body: nil)
    }

    func seestarCleanupStatus() async throws -> SeestarCleanupStatus {
        try await request(path: "/api/seestar/cleanup", method: "GET", body: nil)
    }

    func seestarCleanupAction(_ action: String, token: String) async throws -> SeestarCleanupStatus {
        struct Body: Encodable { let token: String }
        return try await request(path: "/api/seestar/cleanup/\(action)", method: "POST", body: try JSONEncoder().encode(Body(token: token)))
    }

    func cacheAction(_ action: String) async throws -> ActionResponse {
        try await request(path: "/api/fits-cache/\(action)", method: "POST", body: Data("{}".utf8))
    }

    func setCacheDirectory(_ directory: String) async throws -> ActionResponse {
        struct Body: Encodable { let directory: String }
        return try await request(path: "/api/fits-cache/directory", method: "POST", body: try JSONEncoder().encode(Body(directory: directory)))
    }

    private func request<Response: Decodable>(path: String, method: String, body: Data?) async throws -> Response {
        var request = URLRequest(url: baseURL.appendingPathComponent(path))
        request.httpMethod = method
        request.timeoutInterval = 20
        request.setValue(appToken, forHTTPHeaderField: "X-AstroLibrary-App-Token")
        if let body {
            request.httpBody = body
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse, 200..<300 ~= http.statusCode else {
            let message = (try? JSONDecoder().decode(APIErrorResponse.self, from: data).error) ?? "本地服务请求失败"
            throw APIClientError.server(message)
        }
        return try JSONDecoder().decode(Response.self, from: data)
    }
}

enum APIClientError: LocalizedError {
    case server(String)

    var errorDescription: String? {
        switch self {
        case .server(let message): message
        }
    }
}
