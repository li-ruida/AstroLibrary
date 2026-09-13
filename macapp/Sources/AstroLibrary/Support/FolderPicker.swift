import AppKit

@MainActor
enum FolderPicker {
    static func choose(title: String, initialPath: String) -> String? {
        let panel = NSOpenPanel()
        panel.title = title
        panel.prompt = "选择"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = true
        if !initialPath.isEmpty {
            panel.directoryURL = URL(fileURLWithPath: initialPath)
        }
        return panel.runModal() == .OK ? panel.url?.path : nil
    }
}
