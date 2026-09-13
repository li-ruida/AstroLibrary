import SwiftUI

struct CacheSettingsView: View {
    @ObservedObject var model: AppModel
    @State private var directoryDraft: String?
    @State private var directoryMessage = ""
    @State private var directoryFailed = false

    var body: some View {
        Form {
            Section("FIT 解析缓存") {
                if let status = model.cacheStatus {
                    LabeledContent("已缓存", value: "\(size(status.cache.bytes)) · \(status.cache.entries) 条记录")
                    LabeledContent("内存占用", value: size(status.cache.memoryBytes))
                    LabeledContent("磁盘上限", value: size(status.cache.limitBytes))
                    Text(status.cache.path)
                        .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                    if !status.cache.error.isEmpty {
                        Text(status.cache.error).font(.caption).foregroundStyle(.red)
                    }
                } else {
                    ProgressView("正在读取缓存信息…")
                }
                Text("切换图库后自动在后台建立成片 FIT 缓存，跳过 SUB 目录，复用已有有效记录。SUB 图片打开时仍按需解析并缓存。清空缓存不会删除原始照片。")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("缓存目录") {
                HStack {
                    TextField("默认目录（留空使用）", text: Binding(
                        get: { directoryDraft ?? model.cacheStatus?.configuredDirectory ?? "" },
                        set: { directoryDraft = $0; directoryMessage = "" }
                    ))
                    .textFieldStyle(.roundedBorder)
                    Button("选择…") {
                        let current = directoryDraft ?? model.cacheStatus?.configuredDirectory ?? ""
                        if let selected = FolderPicker.choose(title: "选择 FIT 缓存存放目录", initialPath: current) {
                            directoryDraft = selected
                            directoryMessage = ""
                        }
                    }
                }
                HStack {
                    Button("恢复默认目录") { saveDirectory("") }
                    Spacer()
                    Button("保存缓存目录") {
                        saveDirectory(directoryDraft ?? model.cacheStatus?.configuredDirectory ?? "")
                    }
                    .buttonStyle(.borderedProminent)
                }
                Text("建议放在内置 SSD 上。自选目录内会创建 AstroLibrary-FIT-Cache 专用文件夹。保存后立即生效，旧缓存保留在原位置，可切回复用；清空和重建仅作用于当前目录。")
                    .font(.caption).foregroundStyle(.secondary)
                if let status = model.cacheStatus {
                    Text("默认位置：\(status.defaultDirectory)")
                        .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                }
                if !directoryMessage.isEmpty {
                    Text(directoryMessage).font(.caption)
                        .foregroundStyle(directoryFailed ? .red : .secondary)
                }
            }
            .disabled(model.cacheActionPending || model.cacheStatus == nil || model.cacheStatus?.job.running == true)
            Section("重建范围") {
                Text(model.cacheStatus?.job.running == true ? model.cacheStatus!.job.source : model.settings?.libraryPath ?? "尚未连接图库目录")
                    .font(.caption).textSelection(.enabled)
                Text("重建会先清空 FIT 缓存，再处理当前图库目录内的成片 FIT/FITS，跳过 SUB 目录。后台逐张执行，超过容量上限时仍会淘汰旧缓存。")
                    .font(.caption).foregroundStyle(.secondary)
                HStack {
                    Button("清空 FIT 缓存") { Task { await model.runCacheAction("clear") } }
                    Button("重建 FIT 缓存") { Task { await model.runCacheAction("rebuild") } }
                        .buttonStyle(.borderedProminent)
                }
                .disabled(model.cacheActionPending || model.cacheStatus == nil || model.cacheStatus?.job.running == true)
            }
            if let job = model.cacheStatus?.job {
                Section(job.title) {
                    if job.running {
                        if job.phase == "building" && job.total > 0 {
                            ProgressView(value: Double(job.processed), total: Double(job.total))
                        } else {
                            ProgressView().controlSize(.small)
                        }
                    }
                    if ["rebuild", "warmup"].contains(job.operation) {
                        Text("已处理 \(job.processed) / \(job.total)，成功 \(job.processed - job.failed)，失败 \(job.failed)")
                            .font(.callout).monospacedDigit()
                    }
                    if let next = job.nextSource, !next.isEmpty {
                        Text("待建立缓存的图库：\(next)").font(.caption).textSelection(.enabled)
                    }
                    if !job.current.isEmpty {
                        Text(job.current).font(.caption).lineLimit(1).truncationMode(.middle).help(job.current)
                    }
                    if !job.error.isEmpty {
                        Text(job.error).font(.caption).foregroundStyle(.red).textSelection(.enabled)
                    }
                    if job.running && (["rebuild", "warmup"].contains(job.operation) || !(job.nextSource ?? "").isEmpty) {
                        Button("停止缓存任务") { Task { await model.runCacheAction("cancel") } }
                            .disabled(model.cacheActionPending || (job.phase == "stopping" && (job.nextSource ?? "").isEmpty))
                        Text("停止会在当前文件处理结束后生效，已生成的缓存保留。")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            if !model.cacheError.isEmpty {
                Text(model.cacheError).font(.caption).foregroundStyle(.red)
            }
        }
        .formStyle(.grouped)
        .padding(.top, 12)
        .task {
            while !Task.isCancelled {
                await model.refreshCacheStatus()
                do { try await Task.sleep(for: .seconds(model.cacheStatus?.job.running == true ? 1 : 3)) }
                catch { break }
            }
        }
    }

    private func size(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .binary)
    }

    private func saveDirectory(_ directory: String) {
        Task {
            if let error = await model.setCacheDirectory(directory) {
                directoryMessage = error
                directoryFailed = true
            } else {
                directoryDraft = nil
                directoryMessage = "缓存目录已保存并生效。"
                directoryFailed = false
            }
        }
    }
}
