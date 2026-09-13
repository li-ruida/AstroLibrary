import Foundation

struct SeestarImportStatus: Decodable {
    let running: Bool
    let source: String
    let destination: String
    let imported: Int
    let skipped: Int
    let current: String
    let error: String
    let finished: Bool
}

struct SeestarCandidate: Decodable, Identifiable {
    var id: String { path }
    let path: String
    let label: String
    let accessible: Bool
    let hasEntries: Bool
}

struct SeestarCandidates: Decodable {
    let candidates: [SeestarCandidate]
}
