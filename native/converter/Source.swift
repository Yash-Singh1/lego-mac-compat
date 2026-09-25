import Foundation

// A converted Source title. Names match the loader's game profiles and the
// Makefile's GAME=portal2 / GAME=portal bundles.
struct SourceGame: Equatable {
    let displayName: String     // shown in the converter
    let steamFolder: String     // steamapps/common/<steamFolder>
    let executable: String      // launcher at the game root
    let gameDirectory: String   // -game directory holding gameinfo.txt
    let dataDirectory: String   // Contents/SharedSupport/<dataDirectory>
    let appName: String
    let loaderName: String
    let infoPlist: String
    // Local progress and options kept when a bundle is rebuilt.
    let personalPaths: [String]
    var imageName: String { dataDirectory + ".image" }

    static let portal2 = SourceGame(
        displayName: "Portal 2", steamFolder: "Portal 2", executable: "portal2_osx",
        gameDirectory: "portal2", dataDirectory: "Portal2", appName: "Portal2-Compat",
        loaderName: "Portal2Compat", infoPlist: "Info-Portal2.plist",
        personalPaths: ["portal2/SAVE", "portal2/save", "portal2/cfg/config.cfg",
                        "portal2/cfg/video.txt", "portal2/cfg/videodefaults.txt"])
    // The Steam release shares Half-Life 2's hl2_osx launcher.
    static let portal = SourceGame(
        displayName: "Portal", steamFolder: "Portal", executable: "hl2_osx",
        gameDirectory: "portal", dataDirectory: "Portal", appName: "Portal-Compat",
        loaderName: "PortalCompat", infoPlist: "Info-Portal.plist",
        personalPaths: ["portal/SAVE", "portal/save", "portal/cfg/config.cfg",
                        "portal/videoconfig_mac.cfg"])
    static let all = [portal2, portal]
}

// Shared by the GUI converter and Makefile. Depot downloads are overlaid into
// one private game root; no links back to the user's download are introduced.
struct GameSource {
    let game: SourceGame
    let roots: [URL]
    let image: URL
    let resources: URL?
    let icon: URL?

    static func discover(_ selected: URL) throws -> GameSource {
        let fm = FileManager.default
        let selected = selected.resolvingSymlinksInPath().standardizedFileURL
        func exists(_ root: URL, _ path: String) -> Bool {
            fm.fileExists(atPath: root.appendingPathComponent(path).path)
        }
        func content(_ game: SourceGame, _ root: URL) -> Bool {
            exists(root, game.executable) && exists(root, game.gameDirectory + "/gameinfo.txt")
        }
        func client(_ root: URL) -> Bool {
            exists(root, "bin/osx32/launcher.dylib") || exists(root, "bin/launcher.dylib")
        }
        func result(_ game: SourceGame, _ roots: [URL], resources: URL? = nil) -> GameSource {
            let icons = (resources.map { [$0.appendingPathComponent("game.icns")] } ?? []) +
                roots.map { $0.appendingPathComponent(game.gameDirectory + "/resource/game.icns") }
            return GameSource(game: game, roots: roots, image: roots[0].appendingPathComponent(game.executable),
                              resources: resources, icon: icons.first { fm.fileExists(atPath: $0.path) })
        }
        let appRoot = selected.appendingPathComponent("Contents/MacOS")
        if content(.portal2, appRoot) && client(appRoot) {
            let resources = selected.appendingPathComponent("Contents/Resources")
            return result(.portal2, [appRoot], resources: exists(selected, "Contents/Resources") ? resources : nil)
        }
        // Accept the game, common, steamapps, or Steam library folder. Do not
        // crawl unrelated folders or silently combine multiple installations.
        var found: [GameSource] = []
        for game in SourceGame.all {
            for suffix in ["", game.steamFolder, "common/" + game.steamFolder,
                           "steamapps/common/" + game.steamFolder] {
                let root = suffix.isEmpty ? selected : selected.appendingPathComponent(suffix)
                if content(game, root) && client(root) { found.append(result(game, [root])); break }
            }
        }
        if found.count == 1 { return found[0] }
        if found.count > 1 {
            throw ConversionError.message("This folder contains both Portal and Portal 2. Choose the “Portal” or “Portal 2” folder inside steamapps/common.")
        }
        // Only Portal 2 is distributed as separately downloaded depots here.
        let content = { (root: URL) in content(.portal2, root) }
        var depotRoot = selected.appendingPathComponent("depots")
        if !exists(depotRoot, "621") && !exists(depotRoot, "623") {
            depotRoot = selected
            for _ in 0..<3 {
                if exists(depotRoot, "621") || exists(depotRoot, "623") { break }
                depotRoot.deleteLastPathComponent()
            }
        }
        func versions(_ id: String, matching predicate: (URL) -> Bool) -> [URL] {
            let root = depotRoot.appendingPathComponent(id)
            if predicate(root) { return [root] }
            return ((try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil,
                                                options: [.skipsHiddenFiles])) ?? [])
                .filter(predicate).sorted { $0.path < $1.path }
        }
        var common = versions("621", matching: content)
        var mac = versions("623", matching: client)
        // Selecting a specific downloaded version disambiguates that depot.
        if common.contains(where: { $0.resolvingSymlinksInPath().path == selected.path }) { common = [selected] }
        if mac.contains(where: { $0.resolvingSymlinksInPath().path == selected.path }) { mac = [selected] }
        if common.count == 1 && mac.count > 1 {
            mac = mac.filter { $0.lastPathComponent == common[0].lastPathComponent }
        } else if mac.count == 1 && common.count > 1 {
            common = common.filter { $0.lastPathComponent == mac[0].lastPathComponent }
        }
        if common.count == 1 && mac.count == 1 { return result(.portal2, [common[0], mac[0]]) }
        if common.count > 1 || mac.count > 1 {
            throw ConversionError.message("Several Portal 2 depot versions were found. Choose a specific version folder inside depot 621 or 623, or combine the matching depots into one game folder.")
        }
        throw ConversionError.message("Choose a Steam Portal or Portal 2 game folder, Portal 2.app, or the folder containing your downloaded Portal 2 depots. A Portal 2 download needs both the shared game content (depot 621) and the Mac client libraries (depot 623).")
    }

    func checkDestination(_ destination: URL) throws {
        guard !roots.contains(where: { isInside(destination, $0) }) else {
            throw ConversionError.message("Choose an output folder outside the original game files.")
        }
    }

    func copyGame(to destination: URL, check: () throws -> Void = {}) throws {
        let fm = FileManager.default
        func copy(_ source: URL, _ target: URL) throws {
            try check()
            if source.lastPathComponent == ".DepotDownloader" { return }
            let old = try? fm.attributesOfItem(atPath: target.path)
            let attributes = try fm.attributesOfItem(atPath: source.path)
            if attributes[.type] as? FileAttributeType == .typeDirectory {
                if let old, old[.type] as? FileAttributeType != .typeDirectory { try fm.removeItem(at: target) }
                try fm.createDirectory(at: target, withIntermediateDirectories: true)
                for item in try fm.contentsOfDirectory(at: source, includingPropertiesForKeys: nil) {
                    try copy(item, target.appendingPathComponent(item.lastPathComponent))
                }
            } else {
                // Replace links themselves, never write through a copied link
                // when a later depot supplies the same path.
                if old != nil { try fm.removeItem(at: target) }
                try fm.copyItem(at: source, to: target)
            }
        }
        for root in roots { try copy(root, destination) }
    }
}
