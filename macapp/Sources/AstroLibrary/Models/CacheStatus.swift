import Foundation

struct CacheStatus: Decodable {
    let cache: CacheUsage
    let job: CacheJob
    let configuredDirectory: String
    let defaultDirectory: String
}

struct CacheUsage: Decodable {
    let entries: Int
    let bytes: Int64
    let memoryBytes: Int64
    let limitBytes: Int64
    let path: String
    let error: String
}

struct CacheJob: Decodable {
    let running: Bool
    let operation: String
    let phase: String
    let source: String
    let total: Int
    let processed: Int
    let failed: Int
    let current: String
    let error: String

    let nextSource: String?

    var title: String {
        switch phase {
        case "clearing": "正在清空缓存…"
        case "scanning": "正在扫描 FIT 文件…"
        case "building": operation == "warmup" ? "正在为新图库建立缓存" : "正在重建缓存"
        case "stopping": (nextSource ?? "").isEmpty ? "正在停止缓存任务…" : "正在切换到新图库…"
        case "completed": operation == "clear" ? "缓存已清空" : (failed > 0 ? "缓存建立结束，部分文件失败" : "缓存建立完成")
        case "cancelled": "缓存任务已停止"
        case "failed": "缓存操作失败"
        default: "按需缓存已启用"
        }
    }
}
