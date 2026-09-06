import Foundation

// Make uses exactly the same layout discovery and copying as the converter.
@main
struct Portal2SourceTool {
    static func main() {
        do {
            let args = CommandLine.arguments
            guard args.count == 3 || args.count == 4 else {
                throw ConversionError.message("Usage: portal2_source --image SOURCE | --copy SOURCE OUTPUT_APP")
            }
            let source = URL(fileURLWithPath: args[2]).standardizedFileURL
            let layout = try Portal2Source.discover(source)
            if args[1] == "--image" && args.count == 3 { print(layout.image.path); return }
            if args[1] == "--describe" && args.count == 3 {
                let data = try JSONSerialization.data(withJSONObject: ["roots": layout.roots.map(\.path), "image": layout.image.path])
                print(String(decoding: data, as: UTF8.self)); return
            }
            guard args[1] == "--copy" && args.count == 4 else {
                throw ConversionError.message("Expected --image or --copy.")
            }
            let app = URL(fileURLWithPath: args[3]).standardizedFileURL
            try layout.checkDestination(app)
            guard !isInside(app, source) else { throw ConversionError.message("Output must be outside the source.") }
            let fm = FileManager.default
            let output = app.appendingPathComponent("Contents")
            try fm.createDirectory(at: output.appendingPathComponent("Resources"), withIntermediateDirectories: true)
            let game = output.appendingPathComponent("SharedSupport/Portal2")
            let staged = output.appendingPathComponent("SharedSupport/.portal2-source-\(UUID().uuidString)")
            defer { try? fm.removeItem(at: staged) }
            try layout.copyGame(to: staged)
            // Build a fresh tree so switching releases cannot leave old dylibs
            // ahead of the new ones. Retain existing local progress/options.
            for path in Portal2Source.personalPaths {
                let old = game.appendingPathComponent(path)
                guard (try? fm.attributesOfItem(atPath: old.path)) != nil else { continue }
                let new = staged.appendingPathComponent(path)
                try fm.createDirectory(at: new.deletingLastPathComponent(), withIntermediateDirectories: true)
                if (try? fm.attributesOfItem(atPath: new.path)) != nil { try fm.removeItem(at: new) }
                try fm.copyItem(at: old, to: new)
            }
            if (try? fm.attributesOfItem(atPath: game.path)) != nil {
                guard renamex_np(staged.path, game.path, UInt32(RENAME_SWAP)) == 0 else {
                    throw ConversionError.message("Could not replace the game's data: \(String(cString: strerror(errno)))")
                }
            } else { try fm.moveItem(at: staged, to: game) }
            let image = output.appendingPathComponent("SharedSupport/Portal2.image")
            if (try? fm.attributesOfItem(atPath: image.path)) != nil { try fm.removeItem(at: image) }
            try fm.copyItem(at: layout.image, to: image)
            try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: image.path)
            if let resources = layout.resources {
                let worker = Converter(resources: resources) { _, _ in }
                try worker.run("/usr/bin/ditto", ["--noextattr", "--noqtn", resources.path,
                                                output.appendingPathComponent("Resources").path])
            } else if let icon = layout.icon {
                let target = output.appendingPathComponent("Resources/game.icns")
                if (try? fm.attributesOfItem(atPath: target.path)) != nil { try fm.removeItem(at: target) }
                try fm.copyItem(at: icon, to: target)
            }
            print("Prepared Portal 2 from: " + layout.roots.map(\.path).joined(separator: ", "))
        } catch {
            fputs("\(error.localizedDescription)\n", stderr)
            exit(1)
        }
    }
}
