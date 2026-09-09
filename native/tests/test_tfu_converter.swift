import Foundation
import Darwin

private func expect(_ value: @autoclosure () throws -> Bool, _ message: String) throws {
    if try !value() { throw ConversionError.message("FAIL: " + message) }
}

private func rejects(_ description: String, _ body: () throws -> Void) throws {
    do { try body() } catch { return }
    throw ConversionError.message("FAIL: accepted " + description)
}

@main
struct ConverterTests {
    static func main() throws {
        let fm = FileManager.default
        let temp = fm.temporaryDirectory.appendingPathComponent("tfu-tests-\(UUID().uuidString)")
        try fm.createDirectory(at: temp, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: temp) }
        let resources = temp.appendingPathComponent("Converter.app/Contents/Resources")
        try fm.createDirectory(at: resources, withIntermediateDirectories: true)
        let converter = Converter(resources: resources, cache: temp.appendingPathComponent("cache")) { _, _ in }

        let thin = Data([0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 0] + [UInt8](repeating: 0, count: 24))
        func be(_ word: UInt32) -> [UInt8] {
            [UInt8(truncatingIfNeeded: word >> 24), UInt8(truncatingIfNeeded: word >> 16),
             UInt8(truncatingIfNeeded: word >> 8), UInt8(truncatingIfNeeded: word)]
        }
        var fat = Data([0xca, 0xfe, 0xba, 0xbe] + be(2))
        fat.append(contentsOf: be(18) + be(0) + be(48) + be(4) + be(0))
        fat.append(contentsOf: be(7) + be(3) + be(52) + be(UInt32(thin.count)) + be(0))
        fat.append(Data([0, 0, 0, 0]))
        fat.append(thin)
        try expect(try i386Slice(fat) == thin, "extract i386 from a multi-architecture file")
        try expect(try i386Slice(thin) == thin, "accept a thin i386 library")
        for size in 0..<52 {
            try rejects("truncated Mach-O at \(size)") { _ = try i386Slice(Data(fat.prefix(size))) }
        }
        var invalid = fat
        invalid.replaceSubrange(36..<40, with: be(UInt32.max))
        try rejects("out-of-bounds slice") { _ = try i386Slice(invalid) }
        invalid = fat
        invalid.replaceSubrange(36..<40, with: be(0))
        try rejects("overlapping header") { _ = try i386Slice(invalid) }
        invalid = fat
        invalid.replaceSubrange(4..<8, with: be(UInt32.max))
        try rejects("invalid architecture count") { _ = try i386Slice(invalid) }
        invalid = fat
        invalid.replaceSubrange(28..<32, with: be(18))
        try rejects("missing i386") { _ = try i386Slice(invalid) }
        invalid = fat
        invalid[56] = 18
        try rejects("incorrect slice CPU type") { _ = try i386Slice(invalid) }
        print("PASS: Mach-O extraction and malformed input bounds")

        let digestFile = temp.appendingPathComponent("digest")
        try Data("abc".utf8).write(to: digestFile)
        try expect(try converter.sha256(digestFile) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256")

        let source = temp.appendingPathComponent("Star Wars The Force Unleashed.app")
        let destination = temp.appendingPathComponent("outputs")
        try fm.createDirectory(at: source.appendingPathComponent("Contents/MacOS"), withIntermediateDirectories: true)
        try fm.createDirectory(at: destination, withIntermediateDirectories: false)
        try thin.write(to: source.appendingPathComponent("Contents/MacOS/Star Wars The Force Unleashed"))
        let link = temp.appendingPathComponent("source-link")
        try fm.createSymbolicLink(at: link, withDestinationURL: source)
        try expect(isInside(link.appendingPathComponent("Contents"), source), "resolve symlink containment")
        try expect(!isInside(temp.appendingPathComponent("Star Wars The Force Unleashed.app backup"), source), "containment uses path components")
        try rejects("output inside source") { _ = try converter.convert(source: source, destination: link) }
        try rejects("output inside converter") { _ = try converter.convert(source: source, destination: resources) }
        try rejects("missing game") { _ = try converter.convert(source: destination, destination: temp) }
        print("PASS: source and converter cannot be used as output folders")

        func assets(_ root: URL) throws {
            try fm.createDirectory(at: root.appendingPathComponent("LevelPacks"), withIntermediateDirectories: true)
            try fm.createDirectory(at: root.appendingPathComponent("FMV"), withIntermediateDirectories: true)
            try Data("levels".utf8).write(to: root.appendingPathComponent("PC_LevelSelectList.txt"))
            try Data("pack".utf8).write(to: root.appendingPathComponent("LevelPacks/level.bin"))
        }
        try rejects("retail without Assets") { _ = try TFUSource.discover(source) }
        let retailAssets = source.deletingLastPathComponent().appendingPathComponent("Assets")
        try assets(retailAssets)
        let retail = try TFUSource.discover(source)
        try expect(!retail.steam && retail.assets.path == retailAssets.resolvingSymlinksInPath().path, "retail sibling Assets")
        try rejects("output inside retail Assets") { try retail.checkDestination(retailAssets) }
        let retailCopy = temp.appendingPathComponent("retail-copy")
        try retail.copyGame(to: retailCopy)
        try expect(fm.fileExists(atPath: retailCopy.appendingPathComponent("Assets/LevelPacks/level.bin").path), "retail assets copied")
        let steamRoot = temp.appendingPathComponent("SteamLibrary")
        let steamApp = steamRoot.appendingPathComponent("steamapps/common/Star Wars The Force Unleashed/" + TFUSource.appName)
        try fm.createDirectory(at: steamApp.deletingLastPathComponent(), withIntermediateDirectories: true)
        try fm.copyItem(at: source, to: steamApp)
        try assets(steamApp.appendingPathComponent("Contents/GameData"))
        for selection in [steamApp, steamApp.deletingLastPathComponent(),
                          steamRoot.appendingPathComponent("steamapps/common"), steamRoot.appendingPathComponent("steamapps"), steamRoot] {
            let layout = try TFUSource.discover(selection)
            try expect(layout.steam && layout.app.path == steamApp.resolvingSymlinksInPath().path, "Steam app/game/common/library discovery")
        }
        let steam = try TFUSource.discover(steamApp)
        let steamCopy = temp.appendingPathComponent("steam-copy")
        try steam.copyGame(to: steamCopy)
        try expect(fm.fileExists(atPath: steamCopy.appendingPathComponent(TFUSource.appName + "/Contents/GameData/LevelPacks/level.bin").path), "Steam data remains inside app")
        try expect(!fm.fileExists(atPath: steamCopy.appendingPathComponent("Assets").path), "no duplicate Steam data")
        let linked = steamApp.appendingPathComponent("Contents/escape")
        try fm.createSymbolicLink(at: linked, withDestinationURL: destination)
        try rejects("source links") { try steam.copyGame(to: temp.appendingPathComponent("linked-copy")) }
        try fm.removeItem(at: linked)
        print("PASS: Steam/retail discovery, missing assets, containment, private copy, links rejected")

        let existing = destination.appendingPathComponent("TFU-Compat.app")
        try fm.createDirectory(at: existing, withIntermediateDirectories: false)
        let save = existing.appendingPathComponent("SAVE")
        try Data("existing checkpoint".utf8).write(to: save)
        let resultLock = NSLock()
        var outputs: [URL] = []
        var failures: [Error] = []
        DispatchQueue.concurrentPerform(iterations: 12) { index in
            do {
                let stage = temp.appendingPathComponent("stage-\(index)")
                try fm.createDirectory(at: stage, withIntermediateDirectories: false)
                let output = try publish(stage, in: destination)
                resultLock.lock()
                outputs.append(output)
                resultLock.unlock()
            } catch {
                resultLock.lock()
                failures.append(error)
                resultLock.unlock()
            }
        }
        try expect(failures.isEmpty && Set(outputs).count == 12, "concurrent conversions have unique output paths")
        try expect(try String(contentsOf: save, encoding: .utf8) == "existing checkpoint", "existing save not overwritten")
        print("PASS: concurrent output publication preserves existing saves")

        let literal = "Star Wars The Force Unleashed's $(touch SHOULD_NOT_EXIST) `date`; app"
        try expect(try converter.run("/usr/bin/printf", ["%s", literal]) == literal, "paths are passed as literal arguments")
        try rejects("failed child command") { try converter.run("/usr/bin/false", []) }
        let started = Date()
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.2) { converter.cancellation.cancel() }
        do {
            try converter.run("/bin/sleep", ["30"])
            throw ConversionError.message("FAIL: child was not cancelled")
        } catch ConversionError.cancelled {}
        try expect(Date().timeIntervalSince(started) < 5, "cancellation promptly stops child")
        try rejects("cancelled conversion") { _ = try converter.convert(source: source, destination: destination) }
        try expect(try fm.contentsOfDirectory(atPath: destination.path).count == 13, "cancellation creates no unfinished output")
        try expect(try Data(contentsOf: source.appendingPathComponent("Contents/MacOS/Star Wars The Force Unleashed")) == thin, "source remains untouched")
        print("PASS: literal arguments, command failure, cancellation and source preservation")
    }
}
