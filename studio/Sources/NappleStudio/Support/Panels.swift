import AppKit
import UniformTypeIdentifiers

/// Standard open panel. Returns the chosen URLs (empty when cancelled).
@MainActor
func pick(files: Bool, dirs: Bool, multiple: Bool = false, message: String, types: [UTType]? = nil) -> [URL] {
    let p = NSOpenPanel()
    p.canChooseFiles = files; p.canChooseDirectories = dirs; p.allowsMultipleSelection = multiple
    p.message = message
    if let types { p.allowedContentTypes = types }
    return p.runModal() == .OK ? p.urls : []
}
