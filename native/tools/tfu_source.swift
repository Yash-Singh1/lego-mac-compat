import Foundation
import Darwin

@main struct SourceTool {
    static func main() {
        do {
            let args = CommandLine.arguments
            guard args.count >= 3, ["--validate", "--copy"].contains(args[1]) else {
                throw ConversionError.message("Usage: tfu_source --validate SOURCE [ASSETS] | --copy SOURCE OUTPUT [ASSETS]")
            }
            let copying = args[1] == "--copy"
            let base = copying ? 4 : 3
            guard args.count == base || args.count == base + 1 else {
                throw ConversionError.message("Incorrect tfu_source arguments")
            }
            let source = try TFUSource.discover(URL(fileURLWithPath: args[2]),
                assets: args.count > base && !args[base].isEmpty ? URL(fileURLWithPath: args[base]) : nil)
            if copying {
                let output = URL(fileURLWithPath: args[3]).standardizedFileURL
                try source.checkDestination(output)
                let fm = FileManager.default
                let staged = output.deletingLastPathComponent().appendingPathComponent(".tfu-copy-\(UUID().uuidString)")
                defer { try? fm.removeItem(at: staged) }
                try source.copyGame(to: staged)
                if fm.fileExists(atPath: output.path) {
                    guard renamex_np(staged.path, output.path, UInt32(RENAME_SWAP)) == 0 else {
                        throw ConversionError.message("Could not replace the generated TFU data: \(String(cString: strerror(errno)))")
                    }
                } else {
                    guard renamex_np(staged.path, output.path, UInt32(RENAME_EXCL)) == 0 else {
                        throw ConversionError.message("Could not publish the generated TFU data: \(String(cString: strerror(errno)))")
                    }
                }
            } else { print(source.steam ? "TFU Steam Mac 1.3.0" : "TFU retail Mac 1.2") }
        } catch { fputs("\(error.localizedDescription)\n", stderr); exit(1) }
    }
}
