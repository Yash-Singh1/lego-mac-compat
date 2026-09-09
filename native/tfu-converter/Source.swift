import Foundation

// TFU adaptation of portal2's shared source discovery/copy layer (a06456f).
// Steam 1.3 keeps GameData inside the app; retail 1.2 has sibling Assets.
struct TFUSource {
    static let appName = "Star Wars The Force Unleashed.app"
    let app: URL
    let assets: URL
    let steam: Bool
    var image: URL { app.appendingPathComponent("Contents/MacOS/Star Wars The Force Unleashed") }
    var resources: URL { app.appendingPathComponent("Contents/Resources") }
    var roots: [URL] { [app, assets] }

    static func discover(_ selected: URL, assets override: URL? = nil) throws -> TFUSource {
        let fm = FileManager.default
        let selected = selected.resolvingSymlinksInPath().standardizedFileURL
        let suffixes = ["", appName, "Star Wars The Force Unleashed/" + appName,
            "common/Star Wars The Force Unleashed/" + appName,
            "steamapps/common/Star Wars The Force Unleashed/" + appName]
        for suffix in suffixes {
            let app = suffix.isEmpty ? selected : selected.appendingPathComponent(suffix)
            let image = app.appendingPathComponent("Contents/MacOS/Star Wars The Force Unleashed")
            guard fm.fileExists(atPath: image.path) else { continue }
            let data = try Data(contentsOf: image, options: .mappedIfSafe)
            // A different store/build is welcome; the loader checks its code
            // patterns before conversion, rather than whitelisting hashes.
            _ = try i386Slice(data)
            let steam = fm.fileExists(atPath: app.appendingPathComponent("Contents/GameData").path)
            let assets = (override ?? (steam ? app.appendingPathComponent("Contents/GameData") :
                app.deletingLastPathComponent().appendingPathComponent("Assets")))
                .resolvingSymlinksInPath().standardizedFileURL
            for (name, directory) in [("PC_LevelSelectList.txt", false), ("LevelPacks", true), ("FMV", true)] {
                var isDirectory: ObjCBool = false
                guard fm.fileExists(atPath: assets.appendingPathComponent(name).path, isDirectory: &isDirectory),
                      isDirectory.boolValue == directory else {
                    throw ConversionError.message("The TFU game data is incomplete: missing \(name). Steam needs Contents/GameData inside its Mac app; retail needs the Assets folder beside its app. Let Steam finish downloading the Mac installation before converting.")
                }
            }
            return TFUSource(app: app, assets: assets, steam: steam)
        }
        throw ConversionError.message("Choose Star Wars The Force Unleashed.app, its Steam game folder, or its steamapps/common folder. The Windows installation cannot be converted; TFU needs the Mac game files.")
    }

    func checkDestination(_ destination: URL) throws {
        guard !roots.contains(where: { isInside(destination, $0) }) else {
            throw ConversionError.message("Choose an output folder outside the original game files.")
        }
    }

    func copyGame(to destination: URL, check: () throws -> Void = {}) throws {
        let fm = FileManager.default
        func copy(_ source: URL, _ target: URL, relative: String = "") throws {
            try check()
            let attributes = try fm.attributesOfItem(atPath: source.path)
            // Do not preserve links to an installation that may later move, or
            // follow links while finishing/signing the new bundle.
            guard attributes[.type] as? FileAttributeType != .typeSymbolicLink else {
                throw ConversionError.message("The game contains a symbolic link at \(source.path). Choose an original, complete installation.")
            }
            if attributes[.type] as? FileAttributeType == .typeDirectory {
                try fm.createDirectory(at: target, withIntermediateDirectories: true)
                for item in try fm.contentsOfDirectory(at: source, includingPropertiesForKeys: nil) {
                    if relative == "Contents" && item.lastPathComponent == "GameData" {
                        continue // Copy exactly once below, including ASSETS_DIR overrides.
                    }
                    try copy(item, target.appendingPathComponent(item.lastPathComponent),
                             relative: relative.isEmpty ? item.lastPathComponent : relative + "/" + item.lastPathComponent)
                }
            } else {
                try fm.copyItem(at: source, to: target)
            }
        }
        try copy(app, destination.appendingPathComponent(Self.appName))
        let dataOutput = steam ? destination.appendingPathComponent(Self.appName + "/Contents/GameData") :
            destination.appendingPathComponent("Assets")
        try copy(assets, dataOutput)
    }
}
