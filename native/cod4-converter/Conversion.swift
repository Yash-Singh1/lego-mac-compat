import Foundation
import CryptoKit
import Darwin

enum ConversionError: LocalizedError {
    case message(String)
    case cancelled

    var errorDescription: String? {
        switch self {
        case .message(let text): return text
        case .cancelled: return "Conversion cancelled."
        }
    }
}

final class Cancellation {
    private let lock = NSLock()
    private var requested = false

    func cancel() {
        lock.lock()
        requested = true
        lock.unlock()
    }

    func check() throws {
        lock.lock()
        let value = requested
        lock.unlock()
        if value { throw ConversionError.cancelled }
    }
}

// Thin the two downloaded libraries without requiring Xcode's lipo on the
// user's Mac. Fat headers are big endian; the i386 Mach-O slice is little endian.
func machOSlice(_ data: Data, cpu: UInt32) throws -> Data {
    let thinMagic: UInt32 = cpu == 7 ? 0xcefaedfe : 0xcffaedfe
    let headerSize = cpu == 7 ? 28 : 32
    func word(_ offset: Int, little: Bool = false) throws -> UInt32 {
        guard offset >= 0, offset <= data.count - 4 else {
            throw ConversionError.message("The library has a truncated Mach-O header.")
        }
        let bytes = Array(data[offset..<(offset + 4)])
        return (little ? Array(bytes.reversed()) : bytes).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
    }
    let magic = try word(0)
    if magic == thinMagic, data.count >= headerSize, try word(4, little: true) == cpu { return data }
    guard magic == 0xcafebabe else {
        throw ConversionError.message("The library is not an Intel universal library.")
    }
    let count = Int(try word(4))
    guard count <= (data.count - 8) / 20 else {
        throw ConversionError.message("The library has a truncated architecture table.")
    }
    for index in 0..<count {
        let base = 8 + index * 20
        if try word(base) != cpu { continue }
        let offset = Int(try word(base + 8))
        let size = Int(try word(base + 12))
        guard offset >= 8 + count * 20, size >= headerSize,
              offset <= data.count, size <= data.count - offset else {
            throw ConversionError.message("The library has an invalid Intel slice.")
        }
        let slice = data.subdata(in: offset..<(offset + size))
        guard try word(offset) == thinMagic, try word(offset + 4, little: true) == cpu else {
            throw ConversionError.message("The library's Intel slice has an unexpected architecture.")
        }
        return slice
    }
    throw ConversionError.message("The library is missing the required Intel architecture.")
}

func isInside(_ child: URL, _ parent: URL) -> Bool {
    let a = child.resolvingSymlinksInPath().standardizedFileURL.pathComponents
    let b = parent.resolvingSymlinksInPath().standardizedFileURL.pathComponents
    return a.starts(with: b)
}

// RENAME_EXCL makes publishing atomic and never replaces a previous game (and
// its saves), even if another conversion finishes at the same moment.
func publish(_ staged: URL, in directory: URL, name stem: String = "COD4-Compat") throws -> URL {
    for suffix in 1...10000 {
        let name = suffix == 1 ? "\(stem).app" : "\(stem) \(suffix).app"
        let output = directory.appendingPathComponent(name)
        if renamex_np(staged.path, output.path, UInt32(RENAME_EXCL)) == 0 { return output }
        let code = errno
        if code != EEXIST {
            throw ConversionError.message("Could not save the converted app: \(String(cString: strerror(code)))")
        }
    }
    throw ConversionError.message("This folder already contains too many converted copies. Choose another folder.")
}

final class Converter {
    static let downloadURL = "https://updates.cdn-apple.com/2021/macos/041-7683-20210614-E610947E-C7CE-46EB-8860-D26D71F0D3EA/InstallMacOSX.dmg"
    static let installerHash = "db0b2300de719fa3e4ee132b55afd4e689211ad5332760fe5fe7a30351c9e75c"
    static let libraries: [(name: String, member: String, hash: String)] = [
        ("libstdc++.6.dylib", "libstdc++.6.0.9.dylib", "0e68f2c931e50ecf461059a52480610dbb4c8b02a2f28c1f76efee854976fa00"),
        ("libc++abi.dylib", "libc++abi.dylib", "f2c1ebc33f979e7d48a223babcfa679f54f3040390dc022f2beb732ac4acc9af")
    ]
    static var defaultCache: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("org.32bitgoofy.COD4Converter")
    }

    let cancellation = Cancellation()
    let resources: URL
    let cache: URL
    let progress: (String, String) -> Void
    private let fm = FileManager.default

    init(resources: URL, cache: URL = Converter.defaultCache,
         progress: @escaping (String, String) -> Void) {
        self.resources = resources
        self.cache = cache
        self.progress = progress
    }

    func sha256(_ url: URL) throws -> String {
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        var digest = SHA256()
        while true {
            try cancellation.check()
            let chunk = try file.read(upToCount: 1024 * 1024) ?? Data()
            if chunk.isEmpty { break }
            digest.update(data: chunk)
        }
        return digest.finalize().map { String(format: "%02x", $0) }.joined()
    }

    @discardableResult
    func run(_ executable: String, _ arguments: [String], cwd: URL? = nil,
             cancellable: Bool = true, tick: (() -> Void)? = nil) throws -> String {
        if cancellable { try cancellation.check() }
        let log = fm.temporaryDirectory.appendingPathComponent("cod4-command-\(UUID().uuidString).log")
        fm.createFile(atPath: log.path, contents: nil, attributes: [.posixPermissions: 0o600])
        defer { try? fm.removeItem(at: log) }
        let output = try FileHandle(forWritingTo: log)
        defer { try? output.close() }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        process.currentDirectoryURL = cwd
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = output
        process.standardError = output
        try process.run()
        var lastTick = Date.distantPast
        while process.isRunning {
            if cancellable {
                do { try cancellation.check() } catch {
                    process.terminate()
                    let deadline = Date().addingTimeInterval(3)
                    while process.isRunning && Date() < deadline { Thread.sleep(forTimeInterval: 0.05) }
                    if process.isRunning { kill(process.processIdentifier, SIGKILL) }
                    process.waitUntilExit()
                    throw error
                }
            }
            if Date().timeIntervalSince(lastTick) > 0.5 {
                tick?()
                lastTick = Date()
            }
            Thread.sleep(forTimeInterval: 0.1)
        }
        process.waitUntilExit()
        if cancellable { try cancellation.check() }
        let reader = try FileHandle(forReadingFrom: log)
        defer { try? reader.close() }
        let size = try reader.seekToEnd()
        try reader.seek(toOffset: size > 4096 ? size - 4096 : 0)
        let text = String(decoding: try reader.readToEnd() ?? Data(), as: UTF8.self)
        guard process.terminationStatus == 0 else {
            throw ConversionError.message("\(URL(fileURLWithPath: executable).lastPathComponent) could not finish.\n\n\(text.trimmingCharacters(in: .whitespacesAndNewlines))")
        }
        return text
    }

    private func mounted(_ image: URL, at mount: URL, body: () throws -> Void) throws {
        // Even a cancelled attach may already have mounted the image. Always
        // detach our private mount point before removing the extraction folder.
        defer {
            if (try? run("/usr/bin/hdiutil", ["detach", mount.path], cancellable: false)) == nil {
                _ = try? run("/usr/bin/hdiutil", ["detach", "-force", mount.path], cancellable: false)
            }
        }
        try run("/usr/bin/hdiutil", ["attach", "-readonly", "-nobrowse", "-mountpoint", mount.path, image.path])
        try body()
    }

    func prepareRuntime() throws -> URL {
        try fm.createDirectory(at: cache, withIntermediateDirectories: true)
        let lock = open(cache.appendingPathComponent("runtime.lock").path, O_CREAT | O_RDWR | O_CLOEXEC, 0o600)
        guard lock >= 0 else { throw ConversionError.message("Could not open the compatibility download cache.") }
        defer { flock(lock, LOCK_UN); close(lock) }
        while flock(lock, LOCK_EX | LOCK_NB) != 0 {
            guard errno == EWOULDBLOCK else { throw ConversionError.message("Could not lock the compatibility download cache.") }
            try cancellation.check()
            progress("Waiting for another conversion…", "Compatibility files are being prepared by another copy of this app.")
            Thread.sleep(forTimeInterval: 0.2)
        }
        try cancellation.check()
        let runtime = cache.appendingPathComponent("runtime")
        if try Self.libraries.allSatisfy({ library in
            let url = runtime.appendingPathComponent(library.name)
            guard fm.fileExists(atPath: url.path) else { return false }
            return try sha256(url) == library.hash
        }) { return runtime }

        let installer = cache.appendingPathComponent("InstallMacOSX-Lion.dmg")
        progress("Preparing compatibility files…", "The first conversion downloads 4.72 GB from Apple. Later conversions reuse it.")
        if fm.fileExists(atPath: installer.path), try sha256(installer) != Self.installerHash {
            try fm.removeItem(at: installer)
        }
        if !fm.fileExists(atPath: installer.path) {
            let partial = cache.appendingPathComponent("download-\(UUID().uuidString).dmg")
            defer { try? fm.removeItem(at: partial) }
            try run("/usr/bin/curl", ["--fail", "--location", "--proto", "=https", "--proto-redir", "=https",
                                     "--connect-timeout", "30", "--speed-limit", "1024", "--speed-time", "120",
                                     "--retry", "3", "--output", partial.path, Self.downloadURL], tick: {
                let size = (try? self.fm.attributesOfItem(atPath: partial.path)[.size] as? NSNumber)?.int64Value ?? 0
                let downloaded = ByteCountFormatter.string(fromByteCount: size, countStyle: .file)
                self.progress("Downloading compatibility files…", "\(downloaded) of 4.72 GB · from Apple · first conversion only")
            })
            progress("Preparing compatibility files…", "Finishing the download. This can take a moment.")
            guard try sha256(partial) == Self.installerHash else {
                throw ConversionError.message("The Apple download was incomplete or changed. Please try again.")
            }
            try fm.moveItem(at: partial, to: installer)
        }

        progress("Extracting compatibility files…", "Preparing two libraries from the download. Nothing is installed on your Mac.")
        let temp = cache.appendingPathComponent("extract-\(UUID().uuidString)")
        try fm.createDirectory(at: temp, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: temp) }
        let packageImage = temp.appendingPathComponent("InstallMacOSX.pkg/InstallESD.dmg")
        try mounted(installer, at: temp.appendingPathComponent("installer")) {
            try run("/usr/bin/xar", ["-xf", temp.appendingPathComponent("installer/InstallMacOSX.pkg").path,
                                    "InstallMacOSX.pkg/InstallESD.dmg"], cwd: temp)
        }
        try mounted(packageImage, at: temp.appendingPathComponent("esd")) {
            try run("/usr/bin/xar", ["-xf", temp.appendingPathComponent("esd/Packages/BaseSystemBinaries.pkg").path,
                                    "Payload"], cwd: temp)
        }
        try run("/usr/bin/tar", ["-xf", temp.appendingPathComponent("Payload").path] +
                Self.libraries.map { "./usr/lib/" + $0.member }, cwd: temp)
        let staged = temp.appendingPathComponent("runtime")
        try fm.createDirectory(at: staged, withIntermediateDirectories: false)
        for library in Self.libraries {
            try cancellation.check()
            let data = try Data(contentsOf: temp.appendingPathComponent("usr/lib/" + library.member))
            let thin = staged.appendingPathComponent(library.name)
            try machOSlice(data, cpu: 7).write(to: thin)
            guard try sha256(thin) == library.hash else {
                throw ConversionError.message("The downloaded compatibility library \(library.name) did not match the expected version.")
            }
        }
        try "Extracted locally from Apple's Lion archive.\n\(Self.downloadURL)\nArchive SHA-256: \(Self.installerHash)\n"
            .write(to: staged.appendingPathComponent("SOURCE.txt"), atomically: true, encoding: .utf8)
        if fm.fileExists(atPath: runtime.path) { try fm.removeItem(at: runtime) }
        try fm.moveItem(at: staged, to: runtime)
        return runtime
    }

    func convert(source: URL, destination: URL, mode: GameMode = .sp) throws -> URL {
        try cancellation.check()
        let source = source.resolvingSymlinksInPath().standardizedFileURL
        let destination = destination.resolvingSymlinksInPath().standardizedFileURL
        let converterApp = resources.deletingLastPathComponent().deletingLastPathComponent()
        guard !isInside(destination, source),
              !(converterApp.pathExtension == "app" && isInside(destination, converterApp)) else {
            throw ConversionError.message("Choose an output folder outside the original game and the converter app.")
        }
        let layout = try COD4Source.discover(source, mode: mode)
        try layout.checkDestination(destination)
        progress("Checking Call of Duty 4…", "Verifying the Steam Mac \(mode.title.lowercased()) installation.")
        guard try sha256(layout.image) == mode.digest else {
            throw ConversionError.message("This executable differs from the supported Steam Mac 1.7.2 build. Verify the game files in Steam and choose the original Mac installation.")
        }
        let steam = try machOSlice(Data(contentsOf: layout.contents.appendingPathComponent("MacOS/libsteam_api.dylib")), cpu: 0x01000007)
        let loader = resources.appendingPathComponent("game_loader")
        guard fm.isExecutableFile(atPath: loader.path) else {
            throw ConversionError.message("The converter is missing its game launcher. Use a complete copy of COD4-Converter.app.")
        }
        // This also reports a missing Rosetta installation before downloading or copying.
        try run("/usr/bin/arch", ["-x86_64", "/usr/bin/true"])
        let runtime = try prepareRuntime()
        let temp = destination.appendingPathComponent(".cod4-converting-\(UUID().uuidString)")
        try fm.createDirectory(at: temp, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: temp) }
        let app = temp.appendingPathComponent(mode.stem + ".app")
        let output = app.appendingPathComponent("Contents")
        for directory in ["MacOS", "Resources", "SharedSupport"] {
            try fm.createDirectory(at: output.appendingPathComponent(directory), withIntermediateDirectories: true)
        }
        progress("Copying Call of Duty 4…", "Creating your \(mode.title.lowercased()) app. This copies about 7 GB of game files.")
        try copyGameTree(layout.gameData, to: output.appendingPathComponent("Call of Duty 4 Data"), check: cancellation.check)
        try copyGameTree(layout.resources, to: output.appendingPathComponent("Resources"), check: cancellation.check)
        try copyGameTree(layout.image, to: output.appendingPathComponent("SharedSupport/" + mode.imageName), check: cancellation.check)
        try fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: output.appendingPathComponent("SharedSupport/" + mode.imageName).path)
        try copyGameTree(layout.contents.appendingPathComponent("MacOS/libBinkMachOx86.dylib"), to: output.appendingPathComponent("SharedSupport/libBinkMachOx86.dylib"), check: cancellation.check)
        try copyGameTree(runtime, to: output.appendingPathComponent("SharedSupport/compat-runtime"), check: cancellation.check)
        try fm.copyItem(at: loader, to: output.appendingPathComponent("MacOS/" + mode.executable))
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: output.appendingPathComponent("MacOS/" + mode.executable).path)
        let steamOutput = output.appendingPathComponent("Resources/libsteam_api.dylib")
        if fm.fileExists(atPath: steamOutput.path) { try fm.removeItem(at: steamOutput) }
        try steam.write(to: steamOutput)
        var info = try layout.validateLayout()
        info["CFBundleExecutable"] = mode.executable
        info["CFBundleIdentifier"] = "com.aspyr.callofduty4.\(mode.rawValue).steam.compat"
        let name = "Call of Duty 4\(mode == .mp ? " Multiplayer" : "") (Compatibility)"
        info["CFBundleName"] = name
        info["CFBundleDisplayName"] = name
        info["LSMinimumSystemVersion"] = "11.0"
        info["NSHighResolutionCapable"] = false
        info["LP32GeneratedGame"] = "cod4"
        info["LP32ContinueWhenInactive"] = false
        if mode == .mp {
            // Legacy servers choose their own HTTP mod-download hosts. The
            // original NSURLDownload path predates ATS; preserve that support.
            info["NSAppTransportSecurity"] = ["NSAllowsArbitraryLoads": true]
        }
        try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
            .write(to: output.appendingPathComponent("Info.plist"))
        progress("Finishing your app…", "Preparing Call of Duty 4 to open from Finder.")
        try run("/usr/bin/xattr", ["-cr", app.path])
        try run("/usr/bin/codesign", ["--force", "--sign", "-", steamOutput.path])
        try run("/usr/bin/codesign", ["--force", "--sign", "-", app.path])
        try run("/usr/bin/codesign", ["--verify", "--strict", app.path])
        try cancellation.check()
        return try publish(app, in: destination, name: mode.stem)
    }
}
