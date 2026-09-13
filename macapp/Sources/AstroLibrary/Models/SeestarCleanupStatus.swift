import Foundation

struct SeestarCleanupStatus: Decodable {
    let running: Bool
    let phase: String
    let source: String
    let destination: String
    let token: String
    let checked: Int
    let count: Int
    let bytes: Int64
    let skipped: Int
    let moved: Int
    let failed: Int
    let current: String
    let error: String

    var sizeLabel: String { ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file) }
}
