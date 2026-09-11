import Foundation

enum GameMode: String, CaseIterable {
    case sp, mp
    var title: String { self == .sp ? "Campaign" : "Multiplayer" }
    var stem: String { self == .sp ? "COD4-Compat" : "COD4MP-Compat" }
    var executable: String { self == .sp ? "COD4Compat" : "COD4MPCompat" }
    var imageName: String { self == .sp ? "COD4.image" : "COD4MP.image" }
    var originalExecutable: String { self == .sp ? "Call of Duty 4" : "Call of Duty 4 Multiplayer" }
    var icon: String { self == .sp ? "Game.icns" : "Game_mp.icns" }
    var digest: String {
        self == .sp ? "43629c7f2f1b6f891e93c134f3101dbfb9fc4b46cabb1491e90437a450208870" :
            "f90ec11a0822628aac0c2f788d980967ad333405b2fec5f3ecfbca1e3d8d1374"
    }
}

struct COD4Source {
    static let appName = "Call of Duty 4.app"
    let app: URL
    let mode: GameMode
    var contents: URL {
        app.appendingPathComponent(mode == .sp ? "Contents" : "Contents/Call of Duty 4 Multiplayer.app/Contents")
    }
    var image: URL { contents.appendingPathComponent("MacOS/" + mode.originalExecutable) }
    var resources: URL { contents.appendingPathComponent("Resources") }
    var gameData: URL { app.appendingPathComponent("Contents/Call of Duty 4 Data") }

    static func discover(_ selected: URL, mode: GameMode = .sp) throws -> COD4Source {
        let selected = selected.resolvingSymlinksInPath().standardizedFileURL
        var candidates = [selected]
        // Finder can select the nested multiplayer app as well as the outer app.
        if selected.lastPathComponent == "Call of Duty 4 Multiplayer.app" {
            candidates.append(selected.deletingLastPathComponent().deletingLastPathComponent())
        }
        for prefix in ["", "Call of Duty 4/", "common/Call of Duty 4/", "steamapps/common/Call of Duty 4/"] {
            candidates.append(selected.appendingPathComponent(prefix + appName))
        }
        for app in candidates {
            guard FileManager.default.fileExists(atPath: app.appendingPathComponent("Contents/MacOS/Call of Duty 4").path) else { continue }
            let source = COD4Source(app: app, mode: mode)
            _ = try source.validateLayout()
            return source
        }
        throw ConversionError.message("Choose Call of Duty 4.app or its Steam Mac game folder. This converter needs Aspyr’s Steam Mac 1.7.2 release of the original Modern Warfare; Windows, retail and Remastered installations are not supported.")
    }

    func validateLayout() throws -> [String: Any] {
        let fm = FileManager.default
        for file in [image, contents.appendingPathComponent("Info.plist"),
                     resources.appendingPathComponent(mode.icon), contents.appendingPathComponent("MacOS/libBinkMachOx86.dylib"),
                     contents.appendingPathComponent("MacOS/libsteam_api.dylib"), gameData.appendingPathComponent("main/iw_00.iwd")] {
            var directory: ObjCBool = false
            guard fm.fileExists(atPath: file.path, isDirectory: &directory), !directory.boolValue else {
                throw ConversionError.message("The Steam Mac installation is incomplete: missing \(file.lastPathComponent). Let Steam finish installing the Mac version before converting.")
            }
        }
        let data = try Data(contentsOf: contents.appendingPathComponent("Info.plist"))
        guard let info = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              info["CFBundleIdentifier"] as? String == "com.aspyr.callofduty4.\(mode.rawValue).steam",
              info["CFBundleShortVersionString"] as? String == "1.7.2" else {
            throw ConversionError.message("This is not the supported Aspyr Steam Mac 1.7.2 installation. Choose the original Steam app, rather than an already converted copy.")
        }
        return info
    }

    static func steamInstallation(home: URL = FileManager.default.homeDirectoryForCurrentUser) -> URL? {
        let steam = home.appendingPathComponent("Library/Application Support/Steam")
        var libraries = [steam]
        if let text = try? String(contentsOf: steam.appendingPathComponent("steamapps/libraryfolders.vdf"), encoding: .utf8) {
            for path in vdfValues("path", in: text) { libraries.append(URL(fileURLWithPath: path)) }
        }
        for library in libraries {
            guard let manifest = try? String(contentsOf: library.appendingPathComponent("steamapps/appmanifest_7940.acf"), encoding: .utf8),
                  let folder = vdfValues("installdir", in: manifest).first,
                  !folder.isEmpty, folder != ".", folder != "..", !folder.contains("/"), !folder.contains("\\") else { continue }
            let app = library.appendingPathComponent("steamapps/common/" + folder + "/" + appName)
            if (try? discover(app)) != nil { return app }
        }
        return nil
    }

    private static func vdfValues(_ key: String, in text: String) -> [String] {
        let pattern = "\"" + NSRegularExpression.escapedPattern(for: key) + "\"\\s+\"((?:\\\\.|[^\"\\\\])*)\""
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return [] }
        return regex.matches(in: text, range: NSRange(text.startIndex..., in: text)).compactMap {
            guard let range = Range($0.range(at: 1), in: text) else { return nil }
            return String(text[range]).replacingOccurrences(of: "\\\\", with: "\\").replacingOccurrences(of: "\\\"", with: "\"")
        }
    }

    func checkDestination(_ destination: URL) throws {
        guard !isInside(destination, app) else {
            throw ConversionError.message("Choose an output folder outside the original game files.")
        }
    }
}

// Materialize an independent copy. Reject links before any signing or metadata
// changes could follow them back into the Steam installation.
func copyGameTree(_ source: URL, to target: URL, check: () throws -> Void = {}) throws {
    let fm = FileManager.default
    try check()
    let attributes = try fm.attributesOfItem(atPath: source.path)
    switch attributes[.type] as? FileAttributeType {
    case .typeDirectory:
        try fm.createDirectory(at: target, withIntermediateDirectories: true)
        for item in try fm.contentsOfDirectory(at: source, includingPropertiesForKeys: nil) {
            try copyGameTree(item, to: target.appendingPathComponent(item.lastPathComponent), check: check)
        }
    case .typeRegular:
        try fm.copyItem(at: source, to: target)
    default:
        throw ConversionError.message("The installation contains a link or unsupported file at \(source.path). Choose a complete, original Steam installation.")
    }
}
