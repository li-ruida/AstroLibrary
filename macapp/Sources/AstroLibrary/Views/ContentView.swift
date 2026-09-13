import SwiftUI
import AppKit

struct ContentView: View {
    @ObservedObject var model: AppModel
    @Environment(\.openSettings) private var openSettings

    var body: some View {
        Group {
            switch model.serverState {
            case .idle, .starting:
                launchState
            case .ready:
                if let url = model.serverURL {
                    WebGalleryView(url: url, reloadToken: model.reloadToken)
                }
            case .failed(let message):
                failureState(message)
            }
        }
        .frame(minWidth: 900, minHeight: 620)
        .toolbar {
            if #available(macOS 26.0, *) {
                ToolbarSpacer(.flexible)
            }
            ToolbarItem(placement: .primaryAction) {
                Button("设置", systemImage: "gearshape") { openSettings() }
                    .keyboardShortcut(",", modifiers: .command)
                    .help("打开 AstroLibrary 设置")
            }
        }
        .modifier(ModernWindowChrome())
    }

    private var launchState: some View {
        VStack(spacing: 16) {
            ProgressView()
                .controlSize(.large)
            Text(model.launchMessage)
                .font(.headline)
            Text("照片和设置只在这台 Mac 上处理")
                .foregroundStyle(.secondary)
        }
    }

    private func failureState(_ message: String) -> some View {
        ContentUnavailableView {
            Label("无法打开 AstroLibrary", systemImage: "exclamationmark.triangle")
        } description: {
            ScrollView {
                Text(message).textSelection(.enabled).frame(maxWidth: 640, alignment: .leading)
            }.frame(maxHeight: 260)
        } actions: {
            Button("重新检测并启动") {
                Task { await model.retryStart() }
            }
            Button("打开设置") { openSettings() }
            Button("查看服务日志") { NSWorkspace.shared.open(LocalServerController.logURL) }
        }
    }
}

private struct ModernWindowChrome: ViewModifier {
    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(macOS 15.0, *) {
            content
                .toolbar(removing: .title)
        } else {
            content
        }
    }
}
