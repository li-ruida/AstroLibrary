import SwiftUI

struct SeestarCleanupSection: View {
    @ObservedObject var model: AppModel
    let hasUnsavedPath: Bool
    @State private var confirmation: SeestarCleanupStatus?

    private var busy: Bool {
        model.importPending || model.importStatus?.running == true ||
        model.seestarCleanupPending || model.seestarCleanupStatus?.running == true
    }

    var body: some View {
        Section("清理 Seestar 已导入文件") {
            Text("检查 Seestar 下载目录中的照片（含 FIT/FITS 和 SUB）。仅将当前图库中存在同路径、内容完全一致副本的原文件移入废纸篓。")
                .font(.caption).foregroundStyle(.secondary)
            Button("检查可清理文件") { Task { await model.seestarCleanupAction("preview") } }
                .disabled(busy || hasUnsavedPath || (model.settings?.seestarSource ?? "").isEmpty)
            if hasUnsavedPath { Text("请先保存上方的导入目录。").font(.caption).foregroundStyle(.secondary) }
            if let status = model.seestarCleanupStatus, status.phase != "idle" {
                Text("Seestar：\(status.source)\n图库副本：\(status.destination)")
                    .font(.caption).textSelection(.enabled)
                if status.running {
                    HStack {
                        ProgressView().controlSize(.small)
                        Text(status.phase == "checking" ? "正在逐字节核对，已检查 \(status.checked) 个" : "正在移入废纸篓，已处理 \(status.moved + status.failed) 个")
                        Button("停止") { Task { await model.seestarCleanupAction("cancel") } }
                            .disabled(model.seestarCleanupPending)
                    }
                    if !status.current.isEmpty { Text(status.current).font(.caption).lineLimit(1).truncationMode(.middle) }
                }
                if status.phase == "ready" {
                    Text("可清理 \(status.count) 个 · \(status.sizeLabel)，保留 \(status.skipped) 个未通过核对的文件")
                    Button("清理已导入文件…", role: .destructive) { confirmation = status }
                        .disabled(busy || hasUnsavedPath || status.count == 0 || status.source != model.settings?.seestarSource || status.destination != model.settings?.libraryPath)
                }
                if ["completed", "cancelled", "failed"].contains(status.phase) {
                    Text(status.phase == "completed" ? "清理完成" : status.phase == "cancelled" ? "已停止" : "操作未完成")
                    Text("移入废纸篓 \(status.moved) 个，保留 \(status.skipped) 个，失败 \(status.failed) 个")
                        .font(.caption)
                }
                if status.phase == "expired" { Text("检查结果已过期，请重新检查。").font(.caption) }
                if !status.error.isEmpty { Text(status.error).font(.caption).foregroundStyle(.red) }
            }
            if !model.seestarCleanupError.isEmpty { Text(model.seestarCleanupError).font(.caption).foregroundStyle(.red) }
        }
        .confirmationDialog("将已核对的 Seestar 原文件移入废纸篓？", isPresented: Binding(get: { confirmation != nil }, set: { if !$0 { confirmation = nil } }), presenting: confirmation) { status in
            Button("移入废纸篓", role: .destructive) {
                Task { await model.seestarCleanupAction("execute", token: status.token) }
            }
            Button("取消", role: .cancel) { }
        } message: { status in
            Text("共 \(status.count) 个文件（\(status.sizeLabel)）。\n清理目录：\(status.source)\n保留副本：\(status.destination)\n执行前会再次检查文件是否变化。移入废纸篓后，Seestar App 可能仍保留下载记录。")
        }
    }
}
