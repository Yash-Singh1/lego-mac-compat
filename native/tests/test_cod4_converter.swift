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
        let temp = fm.temporaryDirectory.appendingPathComponent("cod4-tests-\(UUID().uuidString)")
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
        try expect(try machOSlice(fat, cpu: 7) == thin, "extract i386 from a multi-architecture file")
        try expect(try machOSlice(thin, cpu: 7) == thin, "accept a thin i386 library")
        for size in 0..<52 {
            try rejects("truncated Mach-O at \(size)") { _ = try machOSlice(Data(fat.prefix(size)), cpu: 7) }
        }
        var invalid = fat
        invalid.replaceSubrange(36..<40, with: be(UInt32.max))
        try rejects("out-of-bounds slice") { _ = try machOSlice(invalid, cpu: 7) }
        invalid = fat
        invalid.replaceSubrange(36..<40, with: be(0))
        try rejects("overlapping header") { _ = try machOSlice(invalid, cpu: 7) }
        invalid = fat
        invalid.replaceSubrange(4..<8, with: be(UInt32.max))
        try rejects("invalid architecture count") { _ = try machOSlice(invalid, cpu: 7) }
        invalid = fat
        invalid.replaceSubrange(28..<32, with: be(18))
        try rejects("missing i386") { _ = try machOSlice(invalid, cpu: 7) }
        invalid = fat
        invalid[56] = 18
        try rejects("incorrect slice CPU type") { _ = try machOSlice(invalid, cpu: 7) }
        print("PASS: Mach-O extraction and malformed input bounds")

        let digestFile = temp.appendingPathComponent("digest")
        try Data("abc".utf8).write(to: digestFile)
        try expect(try converter.sha256(digestFile) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256")

        let thin64 = Data([0xcf, 0xfa, 0xed, 0xfe, 7, 0, 0, 1] + [UInt8](repeating: 0, count: 24))
        var fat64 = fat
        fat64.replaceSubrange(28..<32, with: be(0x01000007))
        fat64.replaceSubrange(52..<fat64.count, with: thin64)
        try expect(try machOSlice(fat64, cpu: 0x01000007) == thin64, "extract x86_64 Steam SDK without lipo")
        try expect(try machOSlice(thin64, cpu: 0x01000007) == thin64, "accept thin x86_64")
        try rejects("wrong Steam SDK architecture") { _ = try machOSlice(thin, cpu: 0x01000007) }

        let steamRoot = temp.appendingPathComponent("Steam Library ' $ nested")
        let source = steamRoot.appendingPathComponent("steamapps/common/Call of Duty 4/Call of Duty 4.app")
        let destination = temp.appendingPathComponent("outputs")
        try fm.createDirectory(at: destination, withIntermediateDirectories: false)
        for mode in GameMode.allCases {
            let layout = COD4Source(app: source, mode: mode)
            for directory in [layout.contents.appendingPathComponent("MacOS"), layout.resources, layout.gameData.appendingPathComponent("main")] {
                try fm.createDirectory(at: directory, withIntermediateDirectories: true)
            }
            for (url, bytes) in [(layout.image, thin), (layout.contents.appendingPathComponent("MacOS/libBinkMachOx86.dylib"), thin),
                                 (layout.contents.appendingPathComponent("MacOS/libsteam_api.dylib"), fat64),
                                 (layout.resources.appendingPathComponent(mode.icon), Data("icon".utf8)),
                                 (layout.gameData.appendingPathComponent("main/iw_00.iwd"), Data("game".utf8))] {
                try bytes.write(to: url)
            }
            let info = ["CFBundleIdentifier": "com.aspyr.callofduty4.\(mode.rawValue).steam", "CFBundleShortVersionString": "1.7.2"]
            try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0).write(to: layout.contents.appendingPathComponent("Info.plist"))
            for selection in [source, source.deletingLastPathComponent(), steamRoot,
                              steamRoot.appendingPathComponent("steamapps"), steamRoot.appendingPathComponent("steamapps/common")] {
                try expect(try COD4Source.discover(selection, mode: mode).image == layout.image, "Steam folder and mode discovery")
            }
            try rejects("unsupported executable fingerprint") { _ = try converter.convert(source: source, destination: destination, mode: mode) }
        }
        let nested = source.appendingPathComponent("Contents/Call of Duty 4 Multiplayer.app")
        try expect(try COD4Source.discover(nested, mode: .mp).app.path == source.path, "nested multiplayer selection")
        let home = temp.appendingPathComponent("home")
        let steam = home.appendingPathComponent("Library/Application Support/Steam/steamapps")
        try fm.createDirectory(at: steam, withIntermediateDirectories: true)
        try "\"libraryfolders\" { \"1\" { \"path\" \"\(steamRoot.path)\" } }".write(to: steam.appendingPathComponent("libraryfolders.vdf"), atomically: true, encoding: .utf8)
        let manifest = steamRoot.appendingPathComponent("steamapps/appmanifest_7940.acf")
        try "\"installdir\" \"Call of Duty 4\"".write(to: manifest, atomically: true, encoding: .utf8)
        try expect(COD4Source.steamInstallation(home: home)?.path == source.path, "external Steam library detected")
        try "\"installdir\" \"../Call of Duty 4\"".write(to: manifest, atomically: true, encoding: .utf8)
        try expect(COD4Source.steamInstallation(home: home) == nil, "manifest traversal rejected")
        let link = temp.appendingPathComponent("source-link")
        try fm.createSymbolicLink(at: link, withDestinationURL: source)
        try expect(isInside(link.appendingPathComponent("Contents"), source), "resolve symlink containment")
        try expect(!isInside(temp.appendingPathComponent("Call of Duty 4.app backup"), source), "containment uses path components")
        try rejects("output inside source") { _ = try converter.convert(source: source, destination: link) }
        try rejects("output inside converter") { _ = try converter.convert(source: source, destination: resources) }
        try rejects("missing game") { _ = try COD4Source.discover(destination) }
        let layout = try COD4Source.discover(source)
        let copy = temp.appendingPathComponent("data-copy")
        try copyGameTree(layout.gameData, to: copy)
        try expect(try Data(contentsOf: copy.appendingPathComponent("main/iw_00.iwd")) == Data("game".utf8), "independent data copy")
        let linked = layout.gameData.appendingPathComponent("escape")
        try fm.createSymbolicLink(at: linked, withDestinationURL: destination)
        try rejects("source links") { try copyGameTree(layout.gameData, to: temp.appendingPathComponent("linked-copy")) }
        try fm.removeItem(at: linked)
        try fm.removeItem(at: layout.gameData.appendingPathComponent("main/iw_00.iwd"))
        try rejects("incomplete data") { _ = try COD4Source.discover(source) }
        try Data("game".utf8).write(to: layout.gameData.appendingPathComponent("main/iw_00.iwd"))
        print("PASS: SP/MP layouts, Steam discovery, fingerprint, copy, links, containment and missing data")

        let existing = destination.appendingPathComponent("COD4-Compat.app")
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

        let literal = "Call of Duty 4's $(touch SHOULD_NOT_EXIST) `date`; app"
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
        try expect(try Data(contentsOf: source.appendingPathComponent("Contents/MacOS/Call of Duty 4")) == thin, "source remains untouched")
        print("PASS: literal arguments, command failure, cancellation and source preservation")
    }
}
