import Foundation

struct AppSettings: Decodable, Equatable {
    var source: String
    var libraryPath: String
    var seestarSource: String
    var destination: String
    var editedSource: String
    var galleryMode: String
    var exportScope: String
    var mergeTargets: Bool
    var port: Int
    var authEnabled: Bool
    var authUsername: String
    var r2: R2Settings
}

struct R2Settings: Decodable, Equatable {
    var enabled: Bool
    var autoUpload: Bool
    var configured: Bool
    var accountId: String
    var bucket: String
    var accessKeyId: String
    var secretConfigured: Bool
    var publicBaseUrl: String
    var keyPrefix: String
    var syncing: Bool
    var mapped: Int
    var uploaded: Int
    var failed: Int

    private enum CodingKeys: String, CodingKey {
        case enabled, autoUpload, configured, accountId, bucket, accessKeyId
        case secretConfigured, publicBaseUrl, keyPrefix, syncing, mapped, uploaded, failed
    }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        enabled = try values.decodeIfPresent(Bool.self, forKey: .enabled) ?? false
        autoUpload = try values.decodeIfPresent(Bool.self, forKey: .autoUpload) ?? false
        configured = try values.decodeIfPresent(Bool.self, forKey: .configured) ?? false
        accountId = try values.decodeIfPresent(String.self, forKey: .accountId) ?? ""
        bucket = try values.decodeIfPresent(String.self, forKey: .bucket) ?? ""
        accessKeyId = try values.decodeIfPresent(String.self, forKey: .accessKeyId) ?? ""
        secretConfigured = try values.decodeIfPresent(Bool.self, forKey: .secretConfigured) ?? false
        publicBaseUrl = try values.decodeIfPresent(String.self, forKey: .publicBaseUrl) ?? ""
        keyPrefix = try values.decodeIfPresent(String.self, forKey: .keyPrefix) ?? "astrolibrary/previews"
        syncing = try values.decodeIfPresent(Bool.self, forKey: .syncing) ?? false
        mapped = try values.decodeIfPresent(Int.self, forKey: .mapped) ?? 0
        uploaded = try values.decodeIfPresent(Int.self, forKey: .uploaded) ?? 0
        failed = try values.decodeIfPresent(Int.self, forKey: .failed) ?? 0
    }
}

struct SettingsUpdate: Encodable {
    let libraryPath: String
    let destination: String
    let editedSource: String
    let galleryMode: String
    let exportScope: String
    let mergeTargets: Bool
    let port: Int
    let r2: R2Update
}

struct R2Update: Encodable {
    let enabled: Bool
    let autoUpload: Bool
    let accountId: String
    let bucket: String
    let accessKeyId: String
    let secretAccessKey: String
    let publicBaseUrl: String
    let keyPrefix: String
}

struct AuthUpdate: Encodable {
    let enabled: Bool
    let username: String
    let password: String
}

struct ActionResponse: Decodable {
    let ok: Bool
    let message: String?
    let started: Bool?
}

struct APIErrorResponse: Decodable {
    let error: String?
}
