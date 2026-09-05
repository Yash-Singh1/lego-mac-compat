// Builds a macOS app icon (.icns) in the standard rounded-square shape from a
// piece of artwork.  Tahoe draws icons that do not fill that shape on a white
// tile, which is how the shipped games' portrait/cut-out artwork ends up as a
// small rectangle with a white border in the Dock.
//
//   swiftc -O mkicon.swift -o mkicon
//   ./mkicon art.png out.icns            # aspect-fill: artwork covers the shape
//   ./mkicon art.png out.icns 20,20,24   # aspect-fit over an RGB background
import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count >= 3 else { fputs("usage: mkicon art.png out.icns [r,g,b]\n", stderr); exit(2) }
let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, nil)!
let art = CGImageSourceCreateImageAtIndex(source, 0, nil)!
var background: CGColor? = nil
if args.count > 3 {
    let c = args[3].split(separator: ",").map { CGFloat(Double($0)!) / 255 }
    background = CGColor(red: c[0], green: c[1], blue: c[2], alpha: 1)
}

// Tight bounds of the opaque artwork.
func opaqueBounds(_ image: CGImage) -> CGRect {
    let w = image.width, h = image.height
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                        space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
    let p = ctx.data!.assumingMemoryBound(to: UInt8.self)
    var minX = w, minY = h, maxX = -1, maxY = -1
    for y in 0..<h { for x in 0..<w where p[(y * w + x) * 4 + 3] > 16 {
        minX = min(minX, x); maxX = max(maxX, x); minY = min(minY, y); maxY = max(maxY, y)
    } }
    return CGRect(x: minX, y: minY, width: maxX - minX + 1, height: maxY - minY + 1)
}
let bounds = opaqueBounds(art)
let cropped = art.cropping(to: bounds)!

func render(_ size: Int) -> CGImage {
    let s = CGFloat(size)
    let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: size * 4,
                        space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.interpolationQuality = .high
    // Apple's icon grid: the rounded square spans 824/1024 of the canvas with a
    // corner radius of 185.4/1024.
    let inset = s * 100 / 1024
    let shape = CGRect(x: inset, y: inset, width: s - 2 * inset, height: s - 2 * inset)
    let path = CGPath(roundedRect: shape, cornerWidth: s * 185.4 / 1024, cornerHeight: s * 185.4 / 1024, transform: nil)
    ctx.addPath(path); ctx.clip()
    let cw = CGFloat(cropped.width), ch = CGFloat(cropped.height)
    if let background = background {
        ctx.setFillColor(background); ctx.fill(shape)
        let pad = shape.width * 0.06
        let fit = min((shape.width - 2 * pad) / cw, (shape.height - 2 * pad) / ch)
        let dw = cw * fit, dh = ch * fit
        ctx.draw(cropped, in: CGRect(x: shape.midX - dw / 2, y: shape.midY - dh / 2, width: dw, height: dh))
    } else {
        let fill = max(shape.width / cw, shape.height / ch)
        let dw = cw * fill, dh = ch * fill
        ctx.draw(cropped, in: CGRect(x: shape.midX - dw / 2, y: shape.midY - dh / 2, width: dw, height: dh))
    }
    return ctx.makeImage()!
}

let out = URL(fileURLWithPath: args[2])
let iconset = out.deletingPathExtension().appendingPathExtension("iconset")
try? FileManager.default.removeItem(at: iconset)
try! FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for (name, size) in [("16x16", 16), ("16x16@2x", 32), ("32x32", 32), ("32x32@2x", 64), ("128x128", 128),
                     ("128x128@2x", 256), ("256x256", 256), ("256x256@2x", 512), ("512x512", 512), ("512x512@2x", 1024)] {
    let dest = CGImageDestinationCreateWithURL(iconset.appendingPathComponent("icon_\(name).png") as CFURL,
                                               UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, render(size), nil)
    CGImageDestinationFinalize(dest)
}
let task = Process()
task.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
task.arguments = ["-c", "icns", iconset.path, "-o", out.path]
try! task.run(); task.waitUntilExit()
try? FileManager.default.removeItem(at: iconset)
exit(task.terminationStatus)
