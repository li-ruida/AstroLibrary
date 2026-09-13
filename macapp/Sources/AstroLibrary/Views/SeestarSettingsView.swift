import SwiftUI

struct SeestarSettingsView: View {
    @ObservedObject var model: AppModel
    @State private var draft: String?

    private var path: String { draft ?? model.settings?.seestarSource ?? "" }
    private var busy: Bool { model.importPending || model.importStatus?.running == true || model.seestarCleanupPending || model.seestarCleanupStatus?.running == true }

    var body: some View {
        Form {
            Section("Seestar 导入（可选）") {
                Text("把 Seestar App 已下载的照片复制到你的图库。图库可以独立使用，不需要安装或连接 Seestar。")
                    .font(.callout).foregroundStyle(.secondary)
                HStack {
                    TextField("Seestar 作品目录", text: Binding(get: { path }, set: { draft = $0 }))
                        .textFieldStyle(.roundedBorder)
                    Button("选择…") {
                        if let selected = FolderPicker.choose(title: "选择 Seestar 已下载照片的目录", initialPath: path) { draft = selected }
                    }
                }
                HStack {
                    Button("自动查找") { Task { await model.detectSeestar() } }
                    Button("保存导入目录") { Task { await model.saveSeestarSource(path) } }
                }
                ForEach(model.seestarCandidates) { candidate in
                    Button { draft = candidate.path } label: {
                        VStack(alignment: .leading) {
                            Text(candidate.label)
                            Text(candidate.path).font(.caption).foregroundStyle(.secondary)
                        }
                    }.disabled(!candidate.accessible)
                }
            }.disabled(busy)

            Section("导入到图库") {
                Text(model.importStatus?.running == true ? model.importStatus!.destination : model.settings?.libraryPath ?? "请先设置图库目录")
                    .font(.caption).textSelection(.enabled)
                Text("复制 FIT/FITS 和照片，包含 SUB，保留子目录结构。同名文件跳过；原文件保留。图库目录在“通用”中设置。")
                    .font(.caption).foregroundStyle(.secondary)
                Button("导入到图库") { Task { await model.saveSeestarSource(path, startImport: true) } }
                    .buttonStyle(.borderedProminent).disabled(busy || path.isEmpty)
            }
            if let status = model.importStatus, status.running || status.finished {
                Section(status.running ? "正在导入…" : status.error.isEmpty ? "导入完成" : "导入未完成") {
                    if status.running { ProgressView().controlSize(.small) }
                    Text("新增 \(status.imported) 个，同名跳过 \(status.skipped) 个")
                    if !status.current.isEmpty { Text(status.current).font(.caption).lineLimit(1).truncationMode(.middle) }
                    if !status.error.isEmpty { Text(status.error).foregroundStyle(.red) }
                }
            }
            SeestarCleanupSection(model: model, hasUnsavedPath: path != (model.settings?.seestarSource ?? ""))
            if !model.importError.isEmpty { Text(model.importError).font(.caption).foregroundStyle(.red) }
        }
        .formStyle(.grouped).padding(.top, 12)
        .task {
            while !Task.isCancelled {
                await model.refreshImportStatus()
                await model.refreshSeestarCleanupStatus()
                do { try await Task.sleep(for: .seconds(1)) } catch { break }
            }
        }
    }
}
