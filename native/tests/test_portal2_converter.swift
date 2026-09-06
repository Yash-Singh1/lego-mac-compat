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
        let temp = fm.temporaryDirectory.resolvingSymlinksInPath().appendingPathComponent("portal2-tests-\(UUID().uuidString)")
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

        let source = temp.appendingPathComponent("Portal 2.app")
        let destination = temp.appendingPathComponent("outputs")
        try fm.createDirectory(at: source.appendingPathComponent("Contents/MacOS"), withIntermediateDirectories: true)
        try fm.createDirectory(at: destination, withIntermediateDirectories: false)
        try thin.write(to: source.appendingPathComponent("Contents/MacOS/portal2_osx"))
        func fixture(_ root: URL, _ path: String, _ value: String = "fixture") throws {
            let file = root.appendingPathComponent(path)
            try fm.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data(value.utf8).write(to: file)
        }
        try fixture(source, "Contents/MacOS/portal2/gameinfo.txt")
        try fixture(source, "Contents/MacOS/bin/launcher.dylib")
        let legacy = try Portal2Source.discover(source)
        try expect(legacy.roots == [source.appendingPathComponent("Contents/MacOS")], "legacy app layout")
        let steam = temp.appendingPathComponent("Steam library ' $")
        let flat = steam.appendingPathComponent("steamapps/common/Portal 2")
        for path in ["portal2_osx", "portal2/gameinfo.txt", "bin/osx32/launcher.dylib"] { try fixture(flat, path) }
        for root in [flat, flat.deletingLastPathComponent(), steam.appendingPathComponent("steamapps"), steam] {
            try expect(try Portal2Source.discover(root).roots.map { $0.resolvingSymlinksInPath().path } == [flat.resolvingSymlinksInPath().path], "Steam folder discovery")
        }
        let download = temp.appendingPathComponent("download")
        let common = download.appendingPathComponent("depots/621/123")
        let mac = download.appendingPathComponent("depots/623/123")
        for path in ["portal2_osx", "portal2/gameinfo.txt", "portal2/resource/game.icns", ".DepotDownloader/staging/partial"] {
            try fixture(common, path)
        }
        try rejects("content without the Mac client") { _ = try Portal2Source.discover(download) }
        try fixture(mac, "bin/osx32/launcher.dylib")
        try fixture(mac, "portal2/bin/osx32/client.dylib")
        for root in [download, download.appendingPathComponent("depots"), common, mac] {
            try expect(try Portal2Source.discover(root).roots.map { $0.resolvingSymlinksInPath().path } == [common.resolvingSymlinksInPath().path, mac.resolvingSymlinksInPath().path], "split depot discovery: \(root.path), got \(try Portal2Source.discover(root).roots.map { $0.resolvingSymlinksInPath().path }), expected \([common.resolvingSymlinksInPath().path, mac.resolvingSymlinksInPath().path])")
        }
        let split = try Portal2Source.discover(download)
        try rejects("destination inside inferred sibling depot") { try split.checkDestination(mac) }
        try expect(split.icon?.resolvingSymlinksInPath().path == common.appendingPathComponent("portal2/resource/game.icns").resolvingSymlinksInPath().path, "flat icon")
        let second = download.appendingPathComponent("depots/623/456")
        try fixture(second, "bin/osx32/launcher.dylib")
        try expect(try Portal2Source.discover(download).roots.map { $0.resolvingSymlinksInPath().path } == [common.resolvingSymlinksInPath().path, mac.resolvingSymlinksInPath().path], "matching depot build")
        let other = download.appendingPathComponent("depots/621/456")
        try fixture(other, "portal2_osx")
        try fixture(other, "portal2/gameinfo.txt")
        try rejects("ambiguous depot versions") { _ = try Portal2Source.discover(download) }
        try expect(try Portal2Source.discover(common).roots.map { $0.resolvingSymlinksInPath().path } == [common.resolvingSymlinksInPath().path, mac.resolvingSymlinksInPath().path], "specific version selection")
        let merged = temp.appendingPathComponent("merged")
        try split.copyGame(to: merged)
        try expect(fm.fileExists(atPath: merged.appendingPathComponent("portal2/bin/osx32/client.dylib").path), "merge client and content directories")
        try expect(!fm.fileExists(atPath: merged.appendingPathComponent(".DepotDownloader").path), "exclude download bookkeeping")
        // A link copied from the common depot must not redirect overlay writes.
        let external = temp.appendingPathComponent("external")
        try fixture(external, "keep", "untouched")
        try fm.createSymbolicLink(at: common.appendingPathComponent("overlay"), withDestinationURL: external)
        try fixture(mac, "overlay/keep", "new copy")
        try split.copyGame(to: merged)
        try expect(try String(contentsOf: external.appendingPathComponent("keep")) == "untouched", "overlay does not follow copied links")
        print("PASS: app, Steam folders, split depots, ambiguity, merge and link preservation")

        let link = temp.appendingPathComponent("source-link")
        try fm.createSymbolicLink(at: link, withDestinationURL: source)
        try expect(isInside(link.appendingPathComponent("Contents"), source), "resolve symlink containment")
        try expect(!isInside(temp.appendingPathComponent("Portal 2.app backup"), source), "containment uses path components")
        try rejects("output inside source") { _ = try converter.convert(source: source, destination: link) }
        try rejects("output inside converter") { _ = try converter.convert(source: source, destination: resources) }
        try rejects("missing game") { _ = try converter.convert(source: destination, destination: temp) }
        print("PASS: source and converter cannot be used as output folders")

        let existing = destination.appendingPathComponent("Portal2-Compat.app")
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

        let literal = "Portal 2's $(touch SHOULD_NOT_EXIST) `date`; app"
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
        try expect(try Data(contentsOf: source.appendingPathComponent("Contents/MacOS/portal2_osx")) == thin, "source remains untouched")
        print("PASS: literal arguments, command failure, cancellation and source preservation")
    }
}
